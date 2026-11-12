#include "krypton/updater/thread_pool.hpp"

namespace kr::updater {

ThreadPool::ThreadPool(std::size_t thread_count) {
  if (thread_count == 0) {
    auto hardware = std::thread::hardware_concurrency();
    thread_count = std::clamp(hardware == 0 ? 2u : hardware, 2u, 8u);
  }

  workers_.reserve(thread_count);
  for (std::size_t i = 0; i < thread_count; ++i) {
    workers_.emplace_back([this]() {
      while (true) {
        std::function<void()> task;
        {
          std::unique_lock<std::mutex> lock(this->mutex_);
          this->cv_.wait(lock, [this]() {
            return this->stop_ || !this->tasks_.empty();
          });
          if (this->stop_ && this->tasks_.empty()) {
            return;
          }
          task = std::move(this->tasks_.front());
          this->tasks_.pop();
        }

        task();

        {
          std::unique_lock<std::mutex> lock(this->mutex_);
          if (--active_tasks_ == 0 && tasks_.empty()) {
            cv_idle_.notify_all();
          }
        }
      }
    });
  }
}

ThreadPool::~ThreadPool() {
  Stop();
}

void ThreadPool::WaitAll() {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_idle_.wait(lock, [this]() {
    return active_tasks_ == 0 && tasks_.empty();
  });
}

void ThreadPool::Stop() {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stop_) return;
    stop_ = true;
  }
  cv_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

}  // namespace kr::updater
