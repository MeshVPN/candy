// SPDX-License-Identifier: MIT
#include "netstack/reactor.h"
#include <spdlog/spdlog.h>

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace candy {

#if defined(__linux__)

static uint32_t toEpollEvents(ReactorEvent events) {
    uint32_t ev = 0;
    if (events & ReactorEvent::READ) {
        ev |= EPOLLIN;
    }
    if (events & ReactorEvent::WRITE) {
        ev |= EPOLLOUT;
    }
    return ev;
}

static ReactorEvent fromEpollEvents(uint32_t ev) {
    ReactorEvent events = ReactorEvent::NONE;
    if (ev & EPOLLIN) {
        events = events | ReactorEvent::READ;
    }
    if (ev & EPOLLOUT) {
        events = events | ReactorEvent::WRITE;
    }
    if (ev & (EPOLLERR | EPOLLHUP)) {
        events = events | ReactorEvent::ERROR;
    }
    return events;
}

Reactor::Reactor() : epollFd(-1), wakeupFd(-1), running(false) {}

Reactor::~Reactor() {
    stop();
}

int Reactor::start() {
    this->epollFd = epoll_create1(EPOLL_CLOEXEC);
    if (this->epollFd < 0) {
        spdlog::error("reactor epoll_create1 failed: {}", strerror(errno));
        return -1;
    }

    this->wakeupFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (this->wakeupFd < 0) {
        spdlog::error("reactor eventfd failed: {}", strerror(errno));
        ::close(this->epollFd);
        this->epollFd = -1;
        return -1;
    }

    struct epoll_event ee = {};
    ee.events = EPOLLIN;
    ee.data.fd = this->wakeupFd;
    if (epoll_ctl(this->epollFd, EPOLL_CTL_ADD, this->wakeupFd, &ee) != 0) {
        spdlog::error("reactor add wakeupFd failed: {}", strerror(errno));
        ::close(this->wakeupFd);
        ::close(this->epollFd);
        this->wakeupFd = -1;
        this->epollFd = -1;
        return -1;
    }

    this->running.store(true);
    this->thread = std::thread([this] { this->loop(); });
    return 0;
}

void Reactor::stop() {
    if (!this->running.exchange(false)) {
        if (this->thread.joinable()) {
            this->thread.join();
        }
        return;
    }
    wakeup();
    if (this->thread.joinable()) {
        this->thread.join();
    }
    if (this->wakeupFd >= 0) {
        ::close(this->wakeupFd);
        this->wakeupFd = -1;
    }
    if (this->epollFd >= 0) {
        ::close(this->epollFd);
        this->epollFd = -1;
    }
}

int Reactor::add(int fd, ReactorEvent events, EventHandler handler) {
    {
        std::unique_lock lock(this->handlerMutex);
        this->handlers[fd] = std::move(handler);
    }
    struct epoll_event ee = {};
    ee.events = toEpollEvents(events);
    ee.data.fd = fd;
    if (epoll_ctl(this->epollFd, EPOLL_CTL_ADD, fd, &ee) != 0) {
        spdlog::error("reactor add fd {} failed: {}", fd, strerror(errno));
        std::unique_lock lock(this->handlerMutex);
        this->handlers.erase(fd);
        return -1;
    }
    return 0;
}

int Reactor::mod(int fd, ReactorEvent events) {
    struct epoll_event ee = {};
    ee.events = toEpollEvents(events);
    ee.data.fd = fd;
    if (epoll_ctl(this->epollFd, EPOLL_CTL_MOD, fd, &ee) != 0) {
        spdlog::error("reactor mod fd {} failed: {}", fd, strerror(errno));
        return -1;
    }
    return 0;
}

int Reactor::del(int fd) {
    epoll_ctl(this->epollFd, EPOLL_CTL_DEL, fd, nullptr);
    std::unique_lock lock(this->handlerMutex);
    this->handlers.erase(fd);
    return 0;
}

void Reactor::post(Task task) {
    {
        std::unique_lock lock(this->taskMutex);
        this->tasks.push(std::move(task));
    }
    wakeup();
}

void Reactor::wakeup() {
    uint64_t one = 1;
    if (this->wakeupFd >= 0) {
        ssize_t n = ::write(this->wakeupFd, &one, sizeof(one));
        (void)n;
    }
}

void Reactor::drainWakeup() {
    uint64_t buf;
    while (::read(this->wakeupFd, &buf, sizeof(buf)) > 0) {
    }
}

void Reactor::drainTasks() {
    std::queue<Task> pending;
    {
        std::unique_lock lock(this->taskMutex);
        std::swap(pending, this->tasks);
    }
    while (!pending.empty()) {
        pending.front()();
        pending.pop();
    }
}

void Reactor::loop() {
    spdlog::debug("start thread: netstack reactor");
    constexpr int MAX_EVENTS = 256;
    struct epoll_event events[MAX_EVENTS];
    while (this->running.load()) {
        int n = epoll_wait(this->epollFd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            spdlog::error("reactor epoll_wait failed: {}", strerror(errno));
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == this->wakeupFd) {
                drainWakeup();
                drainTasks();
                continue;
            }
            EventHandler handler;
            {
                std::unique_lock lock(this->handlerMutex);
                auto it = this->handlers.find(fd);
                if (it == this->handlers.end()) {
                    continue;
                }
                handler = it->second;
            }
            handler(fromEpollEvents(events[i].events));
        }
    }
    spdlog::debug("stop thread: netstack reactor");
}

#else

Reactor::Reactor() : epollFd(-1), wakeupFd(-1), running(false) {}

Reactor::~Reactor() {
    stop();
}

int Reactor::start() {
    spdlog::error("reactor not implemented on this platform yet");
    return -1;
}

void Reactor::stop() {}

int Reactor::add(int, ReactorEvent, EventHandler) {
    return -1;
}

int Reactor::mod(int, ReactorEvent) {
    return -1;
}

int Reactor::del(int) {
    return -1;
}

void Reactor::post(Task) {}

void Reactor::loop() {}

void Reactor::wakeup() {}

void Reactor::drainTasks() {}

void Reactor::drainWakeup() {}

#endif

} // namespace candy
