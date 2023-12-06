#include "FastPipe.h"

#include <malloc.h>
#include <string.h>

#define likely(condition)     __builtin_expect(!!(condition), 1)
#define unlikely(condition)   __builtin_expect(!!(condition), 0)

#define ADD_ABA_TAG(address, tag, addenum, alignment)  ((void*)(((uintptr_t)(address)) | ((((uintptr_t)(tag)) + ((uintptr_t)(addenum))) & (((uintptr_t)(alignment)) - 1ULL))))
#define REMOVE_ABA_TAG(type, address, alignment)       ((type*)(((uintptr_t)(address)) & (~(((uintptr_t)(alignment)) - 1ULL))))

#define ALIGNMENT  64

struct FastPipe* CreateFastPipe(uint32_t granularity, uint32_t threshold, ActivateFastPipeConsumerFunction activate, void* closure)
{
  struct FastPipe* pipe;
  struct FastPipeBaseMessage* message;

  pipe    = (struct FastPipe*)calloc(sizeof(struct FastPipe), 1);
  message = (struct FastPipeBaseMessage*)memalign(ALIGNMENT, sizeof(struct FastPipeBaseMessage) + granularity);

  if ((pipe    == NULL) ||
      (message == NULL))
  {
    free(pipe);
    free(message);
    return NULL;
  }

  memset(message, 0, sizeof(struct FastPipeBaseMessage));

  message->pipe = pipe;
  message->size = malloc_usable_size(message) - offsetof(struct FastPipeBaseMessage, data);

  atomic_init(&pipe->head, message);
  atomic_init(&pipe->tail, message);
  atomic_init(&pipe->count, 1);

  pipe->granularity = granularity;
  pipe->threshold   = threshold;
  pipe->activate    = activate;
  pipe->closure     = closure;

  return pipe;
}

struct FastPipe* HoldFastPipe(struct FastPipe* pipe)
{
  atomic_fetch_add_explicit(&pipe->count, 1, memory_order_relaxed);
  return pipe;
}

void ReleaseFastPipe(struct FastPipe* pipe)
{
  struct FastPipeBaseMessage* next;
  struct FastPipeBaseMessage* current;

  if (unlikely((pipe != NULL) &&
               (atomic_fetch_sub_explicit(&pipe->count, 1, memory_order_relaxed) == 1)))
  {
    // Release unhandled stub messages

    next = atomic_load_explicit(&pipe->tail, memory_order_acquire);

    while (current = next)
    {
      next = current->next;
      free(current);
    }

    // Release messages in pool

    next = atomic_load_explicit(&pipe->pool, memory_order_acquire);

    while (current = REMOVE_ABA_TAG(struct FastPipeBaseMessage, next, ALIGNMENT))
    {
      next = current->next;
      free(current);
    }

    free(pipe);
  }
}

struct FastPipeBaseMessage* AllocateFastPipeMessage(struct FastPipe* pipe, size_t length)
{
  int flag;
  size_t size;
  uint32_t tag;
  void* _Atomic pointer;
  struct FastPipeBaseMessage* message;

  tag  = 0;
  flag = length != 0;
  atomic_fetch_add_explicit(&pipe->count, flag, memory_order_relaxed);

  do pointer = atomic_load_explicit(&pipe->pool, memory_order_acquire);
  while ((message = REMOVE_ABA_TAG(struct FastPipeBaseMessage, pointer, ALIGNMENT)) &&
         (!atomic_compare_exchange_weak_explicit(&pipe->pool, &pointer, message->next, memory_order_relaxed, memory_order_relaxed)));

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

  size = length % pipe->granularity;
  size = length + ((size > 0) || (length == 0)) * (pipe->granularity - size);

  if (likely(message = (struct FastPipeBaseMessage*)memalign(ALIGNMENT, size + offsetof(struct FastPipeBaseMessage, data))))
  {
    message->tag    = tag;
    message->pipe   = pipe;
    message->length = length;
    message->size   = malloc_usable_size(message) - offsetof(struct FastPipeBaseMessage, data);
    atomic_store_explicit(&message->next, NULL, memory_order_release);
    return message;
  }

  atomic_fetch_sub_explicit(&pipe->count, flag, memory_order_relaxed);
  return NULL;
}

void ReleaseFastPipeMessage(struct FastPipeBaseMessage* message)
{
  int flag;
  uint32_t tag;
  struct FastPipe* pipe;

  if (likely(message != NULL))
  {
    pipe = message->pipe;
    flag = message->length != 0;
    tag  = atomic_fetch_add_explicit(&message->tag, 1, memory_order_relaxed) + 1;

    do message->next = atomic_load_explicit(&pipe->pool, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(&pipe->pool, &message->next, ADD_ABA_TAG(message, tag, 0, ALIGNMENT), memory_order_release, memory_order_relaxed));

    if (likely(flag != 0))
    {
      // Any non-stub message increments reference counter
      ReleaseFastPipe(pipe);
    }
  }
}

void SubmitFastPipeMessage(struct FastPipeBaseMessage* message)
{
  int flag;
  struct FastPipe* pipe;
  struct FastPipeBaseMessage* current;

  if (likely(message != NULL))
  {
    pipe    = message->pipe;
    flag    = message->length != 0;
    current = atomic_exchange_explicit(&pipe->head, message, memory_order_acquire);

    atomic_store_explicit(&current->next, message, memory_order_release);

    if (unlikely((flag != 0) &&
                 (atomic_fetch_add_explicit(&pipe->length, 1, memory_order_relaxed) <= pipe->threshold) &&
                 (pipe->activate != NULL)))
    {
      // Kick consumer to activate processing
      pipe->activate(pipe);
    }
  }
}

struct FastPipeBaseMessage* PeekFastPipeMessage(struct FastPipe* pipe)
{
  int flag;
  uintptr_t length;
  struct FastPipeBaseMessage* _Atomic message;

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
      message = AllocateFastPipeMessage(pipe, 0);
      SubmitFastPipeMessage(message);
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
