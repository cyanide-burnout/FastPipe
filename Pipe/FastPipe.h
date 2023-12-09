#ifndef FASTPIPE_H
#define FASTPIPE_H

#include <stddef.h>
#include <stdint.h>

#ifndef __cplusplus

#include <stdatomic.h>

#ifndef ATOMIC
#define ATOMIC(type)  type _Atomic
#endif

#else

#include <atomic>

#ifndef ATOMIC
#define ATOMIC(type)  std::atomic<type>
#endif

extern "C"
{
#endif

#define CAST_FASTPIPE_MESSAGE(type, message)  ((type*)message->data)

struct FastPipe;
struct FastPipeMessage;
struct FastPipeSharedPool;

typedef void (*ActivateFastPipeConsumerFunction)(struct FastPipe* pipe);

struct FastPipeMessage
{
  ATOMIC(struct FastPipeMessage*) next;       // Next message in the queue or pool
  struct FastPipeSharedPool* pool;            // Pool, the message belongs to
  ATOMIC(uint32_t) tag;                       // ABA tag counter
  size_t size;                                // Size of allocation
  size_t length;                              // Size of user data (0 indicates stub message)
  uint8_t data[0];                            // User data
};

struct FastPipeSharedPool
{
  ATOMIC(struct FastPipeMessage*) stack;      // Stacked storage
  ATOMIC(size_t) count;                       // Reference counter
  uint32_t granularity;                       // Granularity of allocation
};

struct FastPipe
{
  ATOMIC(struct FastPipeMessage*) head;       // Queue head
  ATOMIC(struct FastPipeMessage*) tail;       // Queue tail
  struct FastPipeSharedPool* pool;            // Pool used for stub messages
  ATOMIC(intptr_t) length;                    // Enqueued messages count (except stubs)
  ATOMIC(size_t) count;                       // Reference counter
  uint32_t threshold;                         // Amount of messages to avoid consumer activation or stubs adding
  ActivateFastPipeConsumerFunction activate;  // Activation function (or NULL)
  void* closure;                              // User closure
};

struct FastPipeSharedPool* CreateFastPipeSharedPool(uint32_t granularity);
struct FastPipeSharedPool* HoldFastPipeSharedPool(struct FastPipeSharedPool* pool);
void ReleaseFastPipeSharedPool(struct FastPipeSharedPool* pool);

struct FastPipeMessage* AllocateFastPipeMessage(struct FastPipeSharedPool* pool, size_t length);
void ReleaseFastPipeMessage(struct FastPipeMessage* message);

struct FastPipe* CreateFastPipe(struct FastPipeSharedPool* pool, uint32_t threshold, ActivateFastPipeConsumerFunction activate, void* closure);
struct FastPipe* HoldFastPipe(struct FastPipe* pipe);
void ReleaseFastPipe(struct FastPipe* pipe);

void SubmitFastPipeMessage(struct FastPipe* pipe, struct FastPipeMessage* message);
struct FastPipeMessage* PeekFastPipeMessage(struct FastPipe* pipe);

intptr_t GetFastPipeMessageCount(struct FastPipe* pipe);

#ifdef __cplusplus
}
#endif

#endif
