#include "background_worker.h"

#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace {
constexpr std::size_t max_bytes = 128 * 1024 * 1024;
constexpr std::size_t max_jobs = 1024;
}

BackgroundWorker::BackgroundWorker() {
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets_) < 0)
    throw std::system_error(errno, std::generic_category(), "worker socketpair");
  try {
    for (int fd : sockets_) {
      int flags = fcntl(fd, F_GETFL, 0);
      int enabled = 1;
      if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
          fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
          setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
        throw std::system_error(errno, std::generic_category(), "worker socket flags");
    }
    thread_ = std::thread(&BackgroundWorker::run, this);
  } catch (...) {
    close(sockets_[0]);
    close(sockets_[1]);
    throw;
  }
}

BackgroundWorker::~BackgroundWorker() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  available_.notify_one();
  thread_.join();
  close(sockets_[0]);
  close(sockets_[1]);
}

int BackgroundWorker::notification_fd() const { return sockets_[0]; }

bool BackgroundWorker::submit(std::size_t bytes, Work work) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || bytes > max_bytes || bytes_ > max_bytes - bytes ||
        count_ >= max_jobs)
      return false;
    jobs_.push_back({bytes, std::move(work)});
    bytes_ += bytes;
    ++count_;
  }
  available_.notify_one();
  return true;
}

void BackgroundWorker::run() {
  while (true) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      available_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty())
        return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    Completion completion;
    bool failed = false;
    try {
      completion = job.work();
    } catch (...) {
      auto error = std::current_exception();
      completion = [error] { std::rethrow_exception(error); };
      failed = true;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      results_.push_back({job.bytes, std::move(completion)});
      if (failed) {
        jobs_.clear();
        stopping_ = true;
      }
    }
    char byte = 1;
    ssize_t sent;
    do {
      sent = send(sockets_[1], &byte, 1, 0);
    } while (sent < 0 && errno == EINTR);
    if (failed)
      return;
  }
}

void BackgroundWorker::drain() {
  char buffer[1024];
  while (true) {
    auto count = recv(sockets_[0], buffer, sizeof(buffer), 0);
    if (count > 0)
      continue;
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      throw std::system_error(errno, std::generic_category(), "worker notification");
    break;
  }
  std::deque<Result> results;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    results.swap(results_);
    for (const auto &result : results) {
      bytes_ -= result.bytes;
      --count_;
    }
  }
  for (auto &result : results)
    if (result.completion)
      result.completion();
}
