#pragma once
// Minimal readiness poller. epoll on Linux (the target platform); kqueue on
// macOS/BSD so the project still builds and tests on a laptop.
#include <chrono>

namespace gs {

class Poller {
public:
    Poller();
    ~Poller();
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    // Edge-triggered read interest on `fd`.
    void add_readable(int fd);
    // Blocks up to `timeout`; returns number of ready fds (0 on timeout).
    int wait(std::chrono::milliseconds timeout);

private:
    int pfd_ = -1;
};

}  // namespace gs
