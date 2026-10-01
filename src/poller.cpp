#include "gs/poller.hpp"

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#else
#include <sys/event.h>
#endif

namespace gs {

namespace {
[[noreturn]] void throw_errno(const char* what) { throw std::system_error(errno, std::generic_category(), what); }
}  // namespace

#if defined(__linux__)

Poller::Poller() : pfd_(::epoll_create1(EPOLL_CLOEXEC)) {
    if (pfd_ < 0) throw_errno("epoll_create1");
}

void Poller::add_readable(int fd) {
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = fd;
    if (::epoll_ctl(pfd_, EPOLL_CTL_ADD, fd, &ev) < 0) throw_errno("epoll_ctl");
}

int Poller::wait(std::chrono::milliseconds timeout) {
    epoll_event evs[8];
    int n = ::epoll_wait(pfd_, evs, 8, static_cast<int>(timeout.count()));
    if (n < 0 && errno == EINTR) return 0;
    if (n < 0) throw_errno("epoll_wait");
    return n;
}

#else

Poller::Poller() : pfd_(::kqueue()) {
    if (pfd_ < 0) throw_errno("kqueue");
}

void Poller::add_readable(int fd) {
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(pfd_, &ev, 1, nullptr, 0, nullptr) < 0) throw_errno("kevent");
}

int Poller::wait(std::chrono::milliseconds timeout) {
    struct kevent evs[8];
    timespec ts{static_cast<time_t>(timeout.count() / 1000), static_cast<long>((timeout.count() % 1000) * 1'000'000)};
    int n = ::kevent(pfd_, nullptr, 0, evs, 8, &ts);
    if (n < 0 && errno == EINTR) return 0;
    if (n < 0) throw_errno("kevent wait");
    return n;
}

#endif

Poller::~Poller() {
    if (pfd_ >= 0) ::close(pfd_);
}

}  // namespace gs
