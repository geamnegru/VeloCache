#include "event_loop.h"

#include <memory>

std::unique_ptr<EventLoop> make_event_loop() {
    return std::make_unique<EventLoop>();
}
