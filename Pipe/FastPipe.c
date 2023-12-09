#include "FastPipe.h"

#include <malloc.h>
#include <string.h>

#define likely(condition)    __builtin_expect(!!(condition), 1)
#define unlikely(condition)  __builtin_expect(!!(condition), 0)

#define ADD_ABA_TAG(address, tag, addenum, alignment)  ((void*)(((uintptr_t)(address)) | ((((uintptr_t)(tag)) + ((uintptr_t)(addenum))) & (((uintptr_t)(alignment)) - 1ULL))))
#define REMOVE_ABA_TAG(type, address, alignment)       ((type*)(((uintptr_t)(address)) & (~(((uintptr_t)(alignment)) - 1ULL))))

#define ALIGNMENT  64

struct FastPipeSharedPool* CreateFastPipeSharedPool(uint32_t granularity)
{
  struct FastPipeSharedPool* pool;

  if (likely(pool = (struct FastPipeSharedPool*)calloc(sizeof(struct FastPipeSharedPool), 1)))
  {
    pool->count       = 1;
    pool->granularity = granularity;
  }

  return pool;
}

struct FastPipeSharedPool* HoldFastPipeSharedPool(struct FastPipeSharedPool* pool)
{
  atomic_fetch_add_explicit(&pool->count, 1, memory_order_relaxed);
  return pool;
}

void ReleaseFastPipeSharedPool(struct FastPipeSharedPool* pool)
{
  struct FastPipeMessage* next;
  struct FastPipeMessage* current;

  if (unlikely((pool != NULL) &&
               (atomic_fetch_sub_explicit(&pool->count, 1, memory_order_relaxed) == 1)))
  {
    next = atomic_load_explicit(&pool->stack, memory_order_acquire);

    while (current = REMOVE_ABA_TAG(struct FastPipeMessage, next, ALIGNMENT))
    {
      next = current->next;
      free(current);
    }

    free(pool);
  }
}

struct FastPipeMessage* AllocateFastPipeMessage(struct FastPipeSharedPool* pool, size_t length)
{
  size_t size;
  uint32_t tag;
  void* _Atomic pointer;
  struct FastPipeMessage* message;

  tag  = 0;
  atomic_fetch_add_explicit(&pool->count, 1, memory_order_relaxed);

  do pointer = atomic_load_explicit(&pool->stack, memory_order_acquire);
  while ((message = REMOVE_ABA_TAG(struct FastPipeMessage, pointer, ALIGNMENT)) &&
         (!atomic_compare_exchange_weak_explicit(&pool->stack, &pointer, message->next, memory_order_relaxed, memory_order_relaxed)));

  if (likely(message != NULL))
  {
    if (likely(length <= message->size))
    {
      message->length = length;
      atomic_store_explicit(&message->next, NULL, memory_order_release);
      return message;
    }

    tag = message->tag;
    free(message);
  }

  size = length % pool->granularity;
  size = length + ((size > 0) || (length == 0)) * (pool->granularity - size);

  if (likely(message = (struct FastPipeMessage*)memalign(ALIGNMENT, size + offsetof(struct FastPipeMessage, data))))
  {
    message->tag    = tag;
    message->pool   = pool;
    message->length = length;
    message->size   = malloc_usable_size(message) - offsetof(struct FastPipeMessage, data);
    atomic_store_explicit(&message->next, NULL, memory_order_release);
    return message;
  }

  atomic_fetch_sub_explicit(&pool->count, 1, memory_order_relaxed);
  return NULL;
}

void ReleaseFastPipeMessage(struct FastPipeMessage* message)
{
  uint32_t tag;
  struct FastPipeSharedPool* pool;

  if (likely(message != NULL))
  {
    pool = message->pool;
    tag  = atomic_fetch_add_explicit(&message->tag, 1, memory_order_relaxed) + 1;

    do message->next = atomic_load_explicit(&pool->stack, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(&pool->stack, &message->next, ADD_ABA_TAG(message, tag, 0, ALIGNMENT), memory_order_release, memory_order_relaxed));

    ReleaseFastPipeSharedPool(pool);
  }
}

struct FastPipe* CreateFastPipe(struct FastPipeSharedPool* pool, uint32_t threshold, ActivateFastPipeConsumerFunction activate, void* closure)
{
  struct FastPipe* pipe;
  struct FastPipeMessage* message;

  pipe    = (struct FastPipe*)calloc(sizeof(struct FastPipe), 1);
  message = AllocateFastPipeMessage(pool, 0);

  if ((pipe    == NULL) ||
      (message == NULL))
  {
    free(pipe);
    ReleaseFastPipeMessage(message);
    return NULL;
  }

  atomic_init(&pipe->head, message);
  atomic_init(&pipe->tail, message);
  atomic_init(&pipe->count, 1);

  pipe->pool      = HoldFastPipeSharedPool(pool);
  pipe->threshold = threshold;
  pipe->activate  = activate;
  pipe->closure   = closure;

  return pipe;
}

struct FastPipe* HoldFastPipe(struct FastPipe* pipe)
{
  atomic_fetch_add_explicit(&pipe->count, 1, memory_order_relaxed);
  return pipe;
}

void ReleaseFastPipe(struct FastPipe* pipe)
{
  struct FastPipeMessage* next;
  struct FastPipeMessage* current;

  if (unlikely((pipe != NULL) &&
               (atomic_fetch_sub_explicit(&pipe->count, 1, memory_order_relaxed) == 1)))
  {
    next = atomic_load_explicit(&pipe->tail, memory_order_acquire);

    while (current = next)
    {
      next = current->next;
      ReleaseFastPipeMessage(current);
    }

    ReleaseFastPipeSharedPool(pipe->pool);
    free(pipe);
  }
}


void SubmitFastPipeMessage(struct FastPipe* pipe, struct FastPipeMessage* message)
{
  struct FastPipeMessage* current;

  if (likely(message != NULL))
  {
    current = atomic_exchange_explicit(&pipe->head, message, memory_order_acquire);

    atomic_store_explicit(&current->next, message, memory_order_release);

    if (unlikely((message->length != 0) &&
                 (atomic_fetch_add_explicit(&pipe->length, 1, memory_order_relaxed) <= pipe->threshold) &&
                 (pipe->activate != NULL)))
    {
      // Kick consumer to activate processing
      pipe->activate(pipe);
    }
  }
}

struct FastPipeMessage* PeekFastPipeMessage(struct FastPipe* pipe)
{
  uintptr_t length;
  struct FastPipeMessage* _Atomic message;

  if (likely(pipe != NULL))
  {
    // We don't know yet what the message type is
    length = atomic_fetch_sub_explicit(&pipe->length, 1, memory_order_relaxed);

    if (unlikely(length <= 0))
    {
      // No messages are in the queue, restore changes back
      atomic_fetch_add_explicit(&pipe->length, 1, memory_order_relaxed);
      return NULL;
    }

    if (unlikely(length <= pipe->threshold))
    {
      // Always push stub when only few messages are in the queue
      message = AllocateFastPipeMessage(pipe->pool, 0);
      SubmitFastPipeMessage(pipe, message);
    }

    Try:

    message = atomic_exchange_explicit(&pipe->tail, NULL, memory_order_acquire);

    if (unlikely(message == NULL))
    {
      // Tail message already occupied, wait for valid pointer (kind of blocking state)
      goto Try;
    }

    if (unlikely(message->next == NULL))
    {
      // Head is reached somehow, restore changes back
      atomic_store_explicit(&pipe->tail, message, memory_order_relaxed);
      atomic_fetch_add_explicit(&pipe->length, 1, memory_order_relaxed);
      return NULL;
    }

    atomic_store_explicit(&pipe->tail, message->next, memory_order_relaxed);

    __builtin_prefetch(message->next);
    __builtin_prefetch(message->next->data);

    if (unlikely(message->length == 0))
    {
      // Skip all stub messages except final one
      ReleaseFastPipeMessage(message);
      goto Try;
    }

    message->next = NULL;
    return message;
  }

  return NULL;
}

intptr_t GetFastPipeMessageCount(struct FastPipe* pipe)
{
  return likely(pipe) ? atomic_load_explicit(&pipe->length, memory_order_relaxed) : 0;
}
