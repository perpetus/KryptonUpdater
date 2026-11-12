#include <cassert>
#include <iostream>
#include <numeric>
#include <vector>

#include "krypton/updater/thread_pool.hpp"

using namespace kr::updater;

void TestThreadPoolExecution() {
  ThreadPool pool(4);
  assert(pool.ThreadCount() == 4);

  constexpr int kTaskCount = 20;
  std::vector<std::future<int>> futures;
  futures.reserve(kTaskCount);

  for (int i = 0; i < kTaskCount; ++i) {
    futures.push_back(pool.Enqueue([i]() {
      return i * 2;
    }));
  }

  pool.WaitAll();

  int sum = 0;
  for (int i = 0; i < kTaskCount; ++i) {
    sum += futures[i].get();
  }

  // sum of 0*2 + 1*2 + ... + 19*2 = 2 * (19 * 20 / 2) = 380
  assert(sum == 380);
}

int main() {
  TestThreadPoolExecution();
  std::cout << "[PASS] thread_pool_test\n";
  return 0;
}
