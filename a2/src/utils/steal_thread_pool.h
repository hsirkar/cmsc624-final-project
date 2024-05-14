#ifndef _DB_UTILS_STATIC_THREAD_POOL_H_
#define _DB_UTILS_STATIC_THREAD_POOL_H_

#include "assert.h"
#include "pthread.h"
#include "stdlib.h"
#include "utils/atomic.h"
#include "utils/thread_pool.h"
#include <unistd.h>
#include <utility>
#include <vector>

//
class StealThreadPool : public ThreadPool {
public:
  StealThreadPool(int nthreads) : thread_count_(nthreads), stopped_(false) {
    Start();
  }
  ~StealThreadPool() {
    stopped_ = true;
    for (int i = 0; i < thread_count_; i++)
      pthread_join(threads_[i], NULL);
  }

  bool Active() { return !stopped_; }
  virtual void AddTask(Task &&task) {
    assert(!stopped_);
    while (!queues_[rand() % thread_count_].PushNonBlocking(
        std::forward<Task>(task))) {
    }
  }

  virtual void AddTask(const Task &task) {
    assert(!stopped_);
    while (!queues_[rand() % thread_count_].PushNonBlocking(task)) {
    }
  }

  virtual int ThreadCount() { return thread_count_; }

private:
  void Start() {
    threads_.resize(thread_count_);
    queues_.resize(thread_count_);

    pthread_attr_t attr;
    pthread_attr_init(&attr);

#if !defined(_MSC_VER) && !defined(__APPLE__)
    // Pin all threads in the thread pool to CPU Core 0 ~ 6
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int i = 0; i < 7; i++) {
      CPU_SET(i, &cpuset);
    }

    pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset);
#endif

    for (int i = 0; i < thread_count_; i++) {
      pthread_create(&threads_[i], &attr, RunThread,
                     reinterpret_cast<void *>(
                         new std::pair<int, StealThreadPool *>(i, this)));
    }
  }

  // Function executed by each pthread.
  static void *RunThread(void *arg) {
    int queue_id =
        reinterpret_cast<std::pair<int, StealThreadPool *> *>(arg)->first;
    StealThreadPool *tp =
        reinterpret_cast<std::pair<int, StealThreadPool *> *>(arg)->second;

    Task task;
    while (true) {
      Task task;

      for (int i = 0; i < tp->thread_count_; i++) {
        if (tp->queues_[i].PopNonBlocking(&task))
          break;
      }

      if (!task || !tp->queues_[queue_id].Pop(&task))
        break;
      task();
    }
    return NULL;
  }

  int thread_count_;
  std::vector<pthread_t> threads_;

  // Task queues.
  std::vector<AtomicQueue<Task>> queues_;

  bool stopped_;
};

#endif // _DB_UTILS_STATIC_THREAD_POOL_H_
