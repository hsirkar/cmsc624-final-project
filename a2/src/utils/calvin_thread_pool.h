#ifndef _DB_UTILS_CALVIN_THREAD_POOL_H_
#define _DB_UTILS_CALVIN_THREAD_POOL_H_

#include "assert.h"
#include "pthread.h"
#include "stdlib.h"
#include "utils/atomic.h"
#include "utils/thread_pool.h"
#include <queue>
#include <string>
#include <utility>
#include <vector>

using std::pair;
using std::queue;
using std::string;
using std::vector;

//
class CalvinThreadPool : public ThreadPool {
public:
  CalvinThreadPool(int nthreads) : thread_count_(nthreads), stopped_(false) {
    Start();
  }
  ~CalvinThreadPool() {
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
      pthread_create(
          &threads_[i], &attr, RunThread,
          reinterpret_cast<void *>(new pair<int, CalvinThreadPool *>(i, this)));
    }
  }

  // Function executed by each pthread.
  static void *RunThread(void *arg) {
    int queue_id =
        reinterpret_cast<pair<int, CalvinThreadPool *> *>(arg)->first;
    CalvinThreadPool *tp =
        reinterpret_cast<pair<int, CalvinThreadPool *> *>(arg)->second;

    Task task;
    int sleep_duration = 1; // in microseconds
    while (true) {
      if (tp->queues_[queue_id].PopNonBlocking(&task)) {
        task();
        // Reset backoff.
        sleep_duration = 1;
      } else {
        // This part is different from StaticThreadPool.
        // Try to steal work from other queues
        bool stolen = false;
        for (int i = 0; i < tp->thread_count_; i++) {
          if (i != queue_id && tp->queues_[i].PopNonBlocking(&task)) {
            task(); // Do the stolen task
            stolen = true;
            sleep_duration = 1; // Reset backoff.
            break;
          }
        }
        if (!stolen) {
          usleep(sleep_duration);
          // Back off exponentially.
          if (sleep_duration < 32)
            sleep_duration *= 2;
        }
      }

      if (tp->stopped_) {
        // Go through ALL queues looking for a remaining task.
        while (tp->queues_[queue_id].Pop(&task)) {
          task();
        }

        break;
      }
    }
    return NULL;
  }

  int thread_count_;
  vector<pthread_t> threads_;

  // Task queues.
  vector<AtomicQueue<Task>> queues_;

  bool stopped_;
};

#endif // _DB_UTILS_CALVIN_THREAD_POOL_H_
