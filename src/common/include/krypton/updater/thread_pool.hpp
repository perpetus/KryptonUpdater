#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace kr::updater {

class ThreadPool {
 public:
  explicit ThreadPool(std::size_t thread_count = 0);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  template <typename F, typename... Args>
  auto Enqueue(F&& f, Args&&... args) 
      -> std::future<typename std::invoke_result<F, Args...>::type> {
    using return_type = typename std::invoke_result<F, Args...>::type;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        [func = std::forward<F>(f), ... capture_args = std::forward<Args>(args)]() mutable {
          return func(std::forward<Args>(capture_args)...);
        });

    std::future<return_type> res = task->get_future();
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (stop_) {
        throw std::runtime_error("Enqueue on stopped ThreadPool");
      }
      active_tasks_++;
      tasks_.emplace([task]() { (*task)(); });
    }
    cv_.notify_one();
    return res;
  }

  void WaitAll();
  void Stop();
  [[nodiscard]] std::size_t ThreadCount() const noexcept { return workers_.size(); }

 private:
  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable cv_idle_;
  std::atomic<std::size_t> active_tasks_{0};
  bool stop_{false};
};

}  // namespace kr::updater
