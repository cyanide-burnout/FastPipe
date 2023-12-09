#include <stdlib.h>
#include <stdio.h>

#include <poll.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>

#include "FastPipe.h"

struct Context
{
  int handle;
  atomic_int run;
  struct FastPipe* pipe;
  struct FastPipeSharedPool* pool;
};

void activate(struct FastPipe* pipe)
{
  uint64_t value;
  struct Context* context;

  context = (struct Context*)pipe->closure;
  value   = 1ULL;

  write(context->handle, &value, sizeof(uint64_t));
}

void* consumer(void* argument)
{
  uint64_t value;
  pthread_t self;
  struct pollfd event;
  struct Context* context;
  struct FastPipeMessage* message;

  self    = pthread_self();
  context = (struct Context*)argument;

  event.fd      = context->handle;
  event.events  = POLLIN;
  event.revents = 0;

  HoldFastPipeSharedPool(context->pool);
  HoldFastPipe(context->pipe);

  while (atomic_load_explicit(&context->run, memory_order_relaxed))
  {
    poll(&event, 1, 200);

    if (event.revents & POLLIN)
    {
      read(context->handle, &value, sizeof(uint64_t));

      while (message = PeekFastPipeMessage(context->pipe))
      {
        printf("consumer %x - (%p) %s\n", (int)self, message, message->data);
        ReleaseFastPipeMessage(message);
      }

      event.revents = 0;
    }
  }

  ReleaseFastPipeSharedPool(context->pool);
  ReleaseFastPipe(context->pipe);
  return NULL;
}

void* producer(void* argument)
{
  uint64_t value;
  pthread_t self;
  struct Context* context;
  struct FastPipeMessage* message;

  self    = pthread_self();
  context = (struct Context*)argument;
  value   = 0ULL;

  HoldFastPipeSharedPool(context->pool);
  HoldFastPipe(context->pipe);

  while (atomic_load_explicit(&context->run, memory_order_relaxed))
  {
    if (message = AllocateFastPipeMessage(context->pool, 128))
    {
      message->length = snprintf(message->data, 127, "producer %x - number %llu", (int)self, ++ value);
      SubmitFastPipeMessage(context->pipe, message);
    }

    if (GetFastPipeMessageCount(context->pipe) > 120000)
    {
      // Only to avoid out of memory in demo :)
      usleep(1);
    }
  }

  ReleaseFastPipeSharedPool(context->pool);
  ReleaseFastPipe(context->pipe);
  return NULL;
}

int main(int count, char** const arguments)
{
  pthread_t workers[4];
  struct Context context;
  struct FastPipeMessage* message;

  atomic_init(&context.run, 1);

  context.handle = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  context.pool   = CreateFastPipeSharedPool(128);
  context.pipe   = CreateFastPipe(context.pool, 2, activate, &context);

  pthread_create(workers + 0, NULL, consumer, &context);
  pthread_create(workers + 1, NULL, consumer, &context);
  pthread_create(workers + 2, NULL, producer, &context);
  pthread_create(workers + 3, NULL, producer, &context);

  sleep(10);
  atomic_store_explicit(&context.run, 0, memory_order_relaxed);

  pthread_join(workers[0], NULL);
  pthread_join(workers[1], NULL);
  pthread_join(workers[2], NULL);
  pthread_join(workers[3], NULL);

  printf("%d messages left in pipe\n", GetFastPipeMessageCount(context.pipe));

  while (message = PeekFastPipeMessage(context.pipe))
  {
    printf("main - (%p) %s\n", message, message->data);
    ReleaseFastPipeMessage(message);
  }

  ReleaseFastPipeSharedPool(context.pool);
  ReleaseFastPipe(context.pipe);
  close(context.handle);

  return EXIT_SUCCESS;
}
