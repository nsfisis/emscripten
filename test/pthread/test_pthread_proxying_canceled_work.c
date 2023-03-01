#include <assert.h>
#include <emscripten/proxying.h>
#include <pthread.h>
#include <stdio.h>

em_proxying_queue* queue;

void explode(void* arg) { assert(0 && "the work should not be run!"); }

void set_flag(void* flag) {
  // Schedule the flag to be set on the next turn of the event loop so that we
  // can be sure cleanup has finished first. We need to use EM_ASM and JS here
  // because this code needs to run after the thread runtime has exited.

  // clang-format off
  EM_ASM({setTimeout(() => Atomics.store(HEAP32, $0 >> 2, 1))}, flag);
  // clang-format on
}

// Used to call `set_flag` on thread exit or cancellation.
pthread_key_t dtor_key;

void* cancel_self(void* canceled) {
  pthread_setspecific(dtor_key, canceled);
  pthread_cancel(pthread_self());
  pthread_testcancel();
  assert(0 && "thread should have been canceled!");
  return NULL;
}

void* exit_self(void* exited) {
  pthread_setspecific(dtor_key, exited);
  pthread_exit(NULL);
  assert(0 && "thread should have exited!");
  return NULL;
}

void test_cancel_then_proxy() {
  printf("testing cancel followed by proxy\n");

  pthread_t thread;
  _Atomic int canceled = 0;
  pthread_create(&thread, NULL, cancel_self, &canceled);

  // Wait for the thread to be canceled.
  while (!canceled) {
  }

  // Proxying work to the thread should return an error.
  int ret = emscripten_proxy_sync(queue, thread, explode, NULL);
  assert(ret == 0);
  ret =
    emscripten_proxy_callback(queue, thread, explode, explode, explode, NULL);
  assert(ret == 0);

  pthread_join(thread, NULL);
}

void test_exit_then_proxy() {
  printf("testing exit followed by proxy\n");

  pthread_t thread;
  _Atomic int exited = 0;
  pthread_create(&thread, NULL, exit_self, &exited);

  // Wait for the thread to exit.
  while (!exited) {
  }

  // Proxying work to the thread should return an error.
  int ret = emscripten_proxy_sync(queue, thread, explode, NULL);
  assert(ret == 0);
  ret =
    emscripten_proxy_callback(queue, thread, explode, explode, explode, NULL);
  assert(ret == 0);

  pthread_join(thread, NULL);
}

void* wait_then_cancel(void* running) {
  *((_Atomic int*)running) = 1;

  // Wait 20 ms for the proxying to start.
  // TODO: Test with with a cancellable async proxying API to avoid the wait.
  int wait_val = 0;
  int wait_time = 20000000;
  __builtin_wasm_memory_atomic_wait32(&wait_val, 0, wait_time);

  pthread_cancel(pthread_self());
  pthread_testcancel();
  assert(0 && "thread should have been canceled!");
  return NULL;
}

void* wait_then_exit(void* running) {
  *((_Atomic int*)running) = 1;

  // Wait 20 ms for the proxying to start.
  // TODO: Test with with a cancellable async proxying API to avoid the wait.
  int wait_val = 0;
  int wait_time = 20000000;
  __builtin_wasm_memory_atomic_wait32(&wait_val, 0, wait_time);

  pthread_exit(NULL);
  assert(0 && "thread should have exited!");
  return NULL;
}

void test_proxy_then_cancel() {
  printf("testing proxy followed by cancel\n");

  _Atomic int running = 0;
  pthread_t thread;
  pthread_create(&thread, NULL, wait_then_cancel, &running);

  while (!running) {
  }

  // The pending proxied work should be canceled when the thread is canceled.
  int ret = emscripten_proxy_sync(queue, thread, explode, NULL);
  assert(ret == 0);

  pthread_join(thread, NULL);
}

void test_proxy_then_exit() {
  printf("testing proxy followed by exit\n");

  _Atomic int running = 0;
  pthread_t thread;
  pthread_create(&thread, NULL, wait_then_exit, &running);

  while (!running) {
  }

  // The pending proxied work should be canceled when the thread exits.
  int ret = emscripten_proxy_sync(queue, thread, explode, NULL);
  assert(ret == 0);

  pthread_join(thread, NULL);
}

struct callback_info {
  pthread_t worker;
  pthread_t proxier;
  _Atomic int worker_running;
  _Atomic int should_exit;
  _Atomic int callback_called;
};

void* report_running_then_cancel(void* arg) {
  struct callback_info* info = arg;

  info->worker_running = 1;

  while (!info->should_exit) {
  }

  // The callback will never be dequeued because we exit before returning to the
  // event loop.
  pthread_cancel(pthread_self());
  pthread_testcancel();
  assert(0 && "thread should have been canceled!");
  return NULL;
}

void* report_running_then_exit(void* arg) {
  struct callback_info* info = arg;

  info->worker_running = 1;

  while (!info->should_exit) {
  }

  // The callback will never be dequeued because we exit before returning to the
  // event loop.
  pthread_exit(NULL);
  assert(0 && "thread should have been canceled!");
  return NULL;
}

void cancel_callback(void* arg) {
  struct callback_info* info = arg;
  info->callback_called = 1;
}

void* proxy_with_callback(void* arg) {
  struct callback_info* info = arg;

  while (!info->worker_running) {
  }

  int ret = emscripten_proxy_callback(
    queue, info->worker, explode, explode, cancel_callback, info);
  assert(ret == 1);

  info->should_exit = 1;

  // Keep runtime alive so we can receive the cancellation callback.
  emscripten_exit_with_live_runtime();
}

void test_proxy_callback_then_cancel() {
  printf("testing callback proxy followed by cancel\n");

  struct callback_info info = {0};

  pthread_create(&info.worker, NULL, report_running_then_cancel, &info);
  pthread_create(&info.proxier, NULL, proxy_with_callback, &info);

  while (!info.callback_called) {
  }

  pthread_join(info.worker, NULL);

  pthread_cancel(info.proxier);
  pthread_join(info.proxier, NULL);
}

void test_proxy_callback_then_exit() {
  printf("testing callback proxy followed by exit\n");

  struct callback_info info = {0};

  pthread_create(&info.worker, NULL, report_running_then_exit, &info);
  pthread_create(&info.proxier, NULL, proxy_with_callback, &info);

  while (!info.callback_called) {
  }

  pthread_join(info.worker, NULL);

  pthread_cancel(info.proxier);
  pthread_join(info.proxier, NULL);
}

struct in_progress_state {
  int pattern_index;
  int proxier_index;
  _Atomic int running_count;
  pthread_t worker;
  pthread_t proxiers[5];
  em_proxying_ctx* ctxs[5];
  _Atomic int ctx_count;
};

struct state_index {
  int index;
  struct in_progress_state* state;
};

// The patterns of work completion to test so we sufficiently exercise the
// doubly linked list of active contexts. Numbers 1-5 indicate the order in
// which work should be completed and 0 means the work should be canceled.
int patterns[][5] = {{1, 2, 3, 4, 5},
                     {5, 4, 3, 2, 1},
                     {0, 0, 0, 0, 0},
                     {1, 0, 2, 0, 3},
                     {0, 1, 0, 2, 0},
                     {4, 2, 3, 0, 1}};

void receive_ctx(em_proxying_ctx* ctx, void* arg) {
  struct state_index* args = arg;
  args->state->ctxs[args->index] = ctx;
  args->state->ctx_count++;
}

void* in_progress_worker(void* arg) {
  struct in_progress_state* state = arg;

  // Wait to receive all the work contexts.
  while (state->ctx_count < 5) {
    emscripten_proxy_execute_queue(queue);
  }

  // Complete the work in the order specified by the current pattern.
  for (int to_complete = 1; to_complete <= 5; to_complete++) {
    for (int i = 0; i < 5; i++) {
      if (patterns[state->pattern_index][i] == to_complete) {
        printf("finishing task %d\n", i);
        emscripten_proxy_finish(state->ctxs[i]);
      }
    }
  }
  return NULL;
}

void* in_progress_proxier(void* arg) {
  struct in_progress_state* state = arg;
  int index = state->proxier_index;
  state->running_count++;

  struct state_index proxy_args = {index, state};
  int ret = emscripten_proxy_sync_with_ctx(
    queue, state->worker, receive_ctx, &proxy_args);
  // The expected result value depends on the pattern we are executing.
  assert(ret == (0 != patterns[state->pattern_index][index]));
  return NULL;
}

void test_cancel_in_progress() {
  printf("testing cancellation of in-progress work\n");

  int num_patterns = sizeof(patterns) / sizeof(patterns[0]);
  struct in_progress_state state;

  for (state.pattern_index = 0; state.pattern_index < num_patterns;
       state.pattern_index++) {
    state.running_count = 0;
    state.ctx_count = 0;

    printf("checking pattern %d\n", state.pattern_index);

    // Spawn the worker thread.
    pthread_create(&state.worker, NULL, in_progress_worker, &state);

    // Spawn the proxier threads.
    for (state.proxier_index = 0; state.proxier_index < 5;
         state.proxier_index++) {
      pthread_create(&state.proxiers[state.proxier_index],
                     NULL,
                     in_progress_proxier,
                     &state);
      // Wait for the new proxier to start running so it gets the right index.
      while (state.running_count == state.proxier_index) {
      }
    }

    // Wait for all the threads to finish.
    for (int i = 0; i < 5; i++) {
      pthread_join(state.proxiers[i], NULL);
    }
    pthread_join(state.worker, NULL);
  }
}

int main() {
  queue = em_proxying_queue_create();
  pthread_key_create(&dtor_key, set_flag);

  test_cancel_then_proxy();
  test_exit_then_proxy();
  test_proxy_then_cancel();
  test_proxy_then_exit();
  test_proxy_callback_then_cancel();
  test_proxy_callback_then_exit();

  test_cancel_in_progress();

  em_proxying_queue_destroy(queue);

  printf("done\n");
}