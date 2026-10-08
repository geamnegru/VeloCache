#include "background_worker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

void wait_result(BackgroundWorker &worker) {
  pollfd event{worker.notification_fd(), POLLIN, 0};
  require(poll(&event, 1, 3000) > 0, "Worker did not notify completion");
  worker.drain();
}
}

int main() {
  std::ios_base::sync_with_stdio(false);
  std::cin.tie(nullptr);
  try {
    std::mutex mutex;
    std::condition_variable gate;
    bool released = false;
    bool started = false;
    std::vector<int> completed;
    {
      BackgroundWorker worker;
      require(worker.submit(128 * 1024 * 1024, [&] {
        std::unique_lock<std::mutex> lock(mutex);
        started = true;
        gate.notify_one();
        gate.wait(lock, [&] { return released; });
        return [&] { completed.push_back(1); };
      }), "First job was rejected");
      bool ready;
      {
        std::unique_lock<std::mutex> lock(mutex);
        ready = gate.wait_for(lock, std::chrono::seconds(3), [&] { return started; });
      }
      const bool rejected = !worker.submit(1, [] { return [] {}; });
      const bool no_early_completion = completed.empty();
      {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
      }
      gate.notify_one();
      require(ready, "Worker did not start independently");
      require(rejected, "Worker queue exceeded its byte budget");
      require(no_early_completion, "Completion ran before work finished");
      wait_result(worker);
      require(completed == std::vector<int>{1}, "Completion was not delivered");
      for (int i = 2; i < 30; ++i)
        require(worker.submit(1, [&, i] {
          return [&, i] { completed.push_back(i); };
        }), "Queue did not release its byte budget");
      while (completed.size() < 29)
        wait_result(worker);
      for (int i = 1; i < 30; ++i)
        require(completed[static_cast<std::size_t>(i - 1)] == i,
                "FIFO completion order changed");
    }
    {
      BackgroundWorker worker;
      require(worker.submit(1, []() -> BackgroundWorker::Completion {
        throw std::runtime_error("simulated disk failure");
      }), "Failure job rejected");
      bool failed = false;
      try {
        wait_result(worker);
      } catch (const std::runtime_error &error) {
        failed = std::string(error.what()) == "simulated disk failure";
      }
      require(failed, "Worker exception did not reach the event loop");
      require(!worker.submit(1, [] { return [] {}; }), "Failed worker accepted more work");
    }
    std::atomic<int> drained{0};
    {
      BackgroundWorker worker;
      for (int i = 0; i < 20; ++i)
        require(worker.submit(1, [&] {
          ++drained;
          return [] {};
        }), "Shutdown job rejected");
    }
    require(drained == 20, "Shutdown abandoned accepted work");
    std::cout << "PASS background worker FIFO, backpressure, failure and shutdown\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Worker test failed: " << error.what() << '\n';
    return 1;
  }
}
