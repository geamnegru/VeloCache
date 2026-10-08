#ifndef BACKGROUND_WORKER_H
#define BACKGROUND_WORKER_H

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

class BackgroundWorker {
public:
  using Completion = std::function<void()>;
  using Work = std::function<Completion()>;

  BackgroundWorker();
  ~BackgroundWorker();
  BackgroundWorker(const BackgroundWorker &) = delete;
  BackgroundWorker &operator=(const BackgroundWorker &) = delete;

  int notification_fd() const;
  bool submit(std::size_t bytes, Work work);
  void drain();

private:
  struct Job {
    std::size_t bytes;
    Work work;
  };
  struct Result {
    std::size_t bytes;
    Completion completion;
  };

  void run();
  int sockets_[2] = {-1, -1};
  std::mutex mutex_;
  std::condition_variable available_;
  std::deque<Job> jobs_;
  std::deque<Result> results_;
  std::size_t bytes_ = 0;
  std::size_t count_ = 0;
  bool stopping_ = false;
  std::thread thread_;
};

#endif
