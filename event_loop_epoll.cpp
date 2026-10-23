#include "event_loop.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <sys/epoll.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

EventLoop::EventLoop() : queue_fd(epoll_create1(EPOLL_CLOEXEC)) {
    if (queue_fd == -1) {
        throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }
}

EventLoop::~EventLoop() {
    close(queue_fd);
}

void EventLoop::watch(int fd, bool readable, bool writable) {
    struct epoll_event ev{};
    uint32_t events = 0;
    if (readable) events |= EPOLLIN;
    if (writable) events |= EPOLLOUT;
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(queue_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        if (errno == EEXIST) {
            if (epoll_ctl(queue_fd, EPOLL_CTL_MOD, fd, &ev) == -1) {
                throw std::system_error(errno, std::generic_category(), "epoll_ctl mod");
            }
            return;
        }
        throw std::system_error(errno, std::generic_category(), "epoll_ctl add");
    }
}

void EventLoop::remove(int fd) {
    if (epoll_ctl(queue_fd, EPOLL_CTL_DEL, fd, nullptr) == -1) {
        if (errno != ENOENT && errno != EBADF) {
            throw std::system_error(errno, std::generic_category(), "epoll_ctl del");
        }
    }
}

std::vector<SocketEvent> EventLoop::wait(int timeout_ms) {
    std::array<struct epoll_event, 128> events;

    const int count = epoll_wait(
        queue_fd,
        events.data(),
        static_cast<int>(events.size()),
        timeout_ms);

    if (count == -1) {
        if (errno == EINTR) {
            return {};
        }
        throw std::system_error(errno, std::generic_category(), "epoll_wait");
    }

    std::vector<SocketEvent> result;
    std::unordered_map<int, std::size_t> positions;
    result.reserve(static_cast<std::size_t>(count));
    positions.reserve(static_cast<std::size_t>(count));

    for (int index = 0; index < count; ++index) {
        const auto &event = events[static_cast<std::size_t>(index)];
        const int fd = event.data.fd;
        const auto entry = positions.emplace(fd, result.size());

        if (entry.second) {
            result.push_back({fd, false, false, false});
        }

        auto &socket = result[entry.first->second];
        socket.readable = socket.readable || (event.events & EPOLLIN) != 0;
        socket.writable = socket.writable || (event.events & EPOLLOUT) != 0;
        socket.failed = socket.failed || (event.events & (EPOLLERR | EPOLLHUP)) != 0;
    }

    return result;
}
