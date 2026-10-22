#include "event_loop.h"

#include <memory>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#include "event_loop_kqueue.cpp"
#else
#include "event_loop_epoll.cpp"
#endif

std::unique_ptr<EventLoop> make_event_loop() {
    return std::make_unique<EventLoop>();
}