#include "event_loop.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <unordered_map>

EventLoop::EventLoop() : queue_fd(kqueue()) {
    if (queue_fd == -1) {
        throw std::system_error(errno, std::generic_category(), "kqueue");
    }

    int flags;
    do {
        flags = fcntl(queue_fd, F_GETFD);
    } while (flags == -1 && errno == EINTR);

    int result = -1;
    if (flags != -1) {
        do {
            result = fcntl(queue_fd, F_SETFD, flags | FD_CLOEXEC);
        } while (result == -1 && errno == EINTR);
    }

    if (result == -1) {
        const int error = errno;
        close(queue_fd);
        throw std::system_error(error, std::generic_category(), "fcntl kqueue");
    }
}

EventLoop::~EventLoop() {
    close(queue_fd);
}

void EventLoop::watch(int fd, bool readable, bool writable) {
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ,
           EV_ADD | (readable ? EV_ENABLE : EV_DISABLE), 0, 0, nullptr);
    EV_SET(&changes[1], fd, EVFILT_WRITE,
           EV_ADD | (writable ? EV_ENABLE : EV_DISABLE), 0, 0, nullptr);

    int result;
    do {
        result = kevent(queue_fd, changes, 2, nullptr, 0, nullptr);
    } while (result == -1 && errno == EINTR);

    if (result == -1) {
        throw std::system_error(errno, std::generic_category(), "kevent watch");
    }
}

void EventLoop::remove(int fd) {
    for (const short filter : {EVFILT_READ, EVFILT_WRITE}) {
        struct kevent change;
        EV_SET(&change, fd, filter, EV_DELETE, 0, 0, nullptr);

        int result;
        do {
            result = kevent(queue_fd, &change, 1, nullptr, 0, nullptr);
        } while (result == -1 && errno == EINTR);

        if (result == -1 && errno != ENOENT && errno != EBADF) {
            throw std::system_error(errno, std::generic_category(), "kevent remove");
        }
    }
}

std::vector<SocketEvent> EventLoop::wait(int timeout_ms) {
    std::array<struct kevent, 128> events;
    struct timespec timeout {};
    if (timeout_ms >= 0) {
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_nsec = (timeout_ms % 1000) * 1000000L;
    }

    const int count = kevent(queue_fd, nullptr, 0, events.data(),
                             static_cast<int>(events.size()),
                             timeout_ms < 0 ? nullptr : &timeout);
    if (count == -1) {
        if (errno == EINTR) {
            return {};
        }
        throw std::system_error(errno, std::generic_category(), "kevent wait");
    }

    std::vector<SocketEvent> result;
    std::unordered_map<int, std::size_t> positions;
    result.reserve(static_cast<std::size_t>(count));
    positions.reserve(static_cast<std::size_t>(count));

    for (int index = 0; index < count; ++index) {
        const auto& event = events[static_cast<std::size_t>(index)];
        const int fd = static_cast<int>(event.ident);
        const auto entry = positions.emplace(fd, result.size());
        if (entry.second) {
            result.push_back({fd, false, false, false});
        }

        auto& socket = result[entry.first->second];
        socket.readable = socket.readable || event.filter == EVFILT_READ;
        socket.writable = socket.writable || event.filter == EVFILT_WRITE;
        socket.failed = socket.failed || (event.flags & EV_ERROR) != 0 ||
                        (event.filter == EVFILT_WRITE && (event.flags & EV_EOF) != 0);
    }

    return result;
}
