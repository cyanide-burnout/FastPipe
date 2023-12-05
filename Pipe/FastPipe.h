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
struct FastPipeBaseMessage;

typedef void (*ActivateFastPipeConsumerFunction)(struct FastPipe* pipe);

struct FastPipeBaseMessage
{
  ATOMIC(struct FastPipeBaseMessage*) next;   // Next message in the queue or pool
  ATOMIC(uint32_t) tag;                       // ABA tag counter
  struct FastPipe* pipe;                      // Pipe, the message belongs to
  size_t size;                                // Size of allocation
  size_t length;                              // Size of user data (0 indicates stub message)
  uint8_t data[0];                            // User data
};

struct FastPipe
{
  ATOMIC(struct FastPipeBaseMessage*) head;   // Queue head
  ATOMIC(struct FastPipeBaseMessage*) tail;   // Queue tail
  ATOMIC(struct FastPipeBaseMessage*) pool;   // Pool of free messages
  ATOMIC(intptr_t) length;                    // Enqueued messages count (except stubs)
  ATOMIC(size_t) count;                       // Reference counter
  uint32_t granularity;                       // Granularity of allocation
  uint32_t threshold;                         // Amount of messages to avoid consumer activation or stubs adding
  ActivateFastPipeConsumerFunction activate;  // Activation function (or NULL)
  void* closure;                              // User closure
};

struct FastPipe* CreateFastPipe(uint32_t granularity, uint32_t threshold, ActivateFastPipeConsumerFunction activate, void* closure);
struct FastPipe* HoldFastPipe(struct FastPipe* pipe);
void ReleaseFastPipe(struct FastPipe* pipe);

struct FastPipeBaseMessage* AllocateFastPipeMessage(struct FastPipe* pipe, size_t length);
void ReleaseFastPipeMessage(struct FastPipeBaseMessage* message);

void SubmitFastPipeMessage(struct FastPipeBaseMessage* message);
struct FastPipeBaseMessage* PeekFastPipeMessage(struct FastPipe* pipe);

intptr_t GetFastPipeMessageCount(struct FastPipe* pipe);

#ifdef __cplusplus
}
#endif

#endif
