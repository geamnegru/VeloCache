#ifndef VELOCACHE_EVENT_LOOP_H
#define VELOCACHE_EVENT_LOOP_H

#include <vector>

struct SocketEvent {
    int fd;
    bool readable;
    bool writable;
    bool failed;
};

class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void watch(int fd, bool readable, bool writable);
    void remove(int fd);
    std::vector<SocketEvent> wait(int timeout_ms);

private:
    int queue_fd;
};

#endif
