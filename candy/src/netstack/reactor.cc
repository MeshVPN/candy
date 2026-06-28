// SPDX-License-Identifier: MIT
#include "netstack/reactor.h"
#include <spdlog/spdlog.h>

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(_WIN32) || defined(_WIN64)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>
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

#elif defined(__APPLE__)

// EVFILT_USER 的标识，用于跨线程唤醒 reactor 线程（替代 Linux 的 eventfd）。
static constexpr uintptr_t WAKEUP_IDENT = 1;

Reactor::Reactor() : kqueueFd(-1), running(false) {}

Reactor::~Reactor() {
    stop();
}

int Reactor::start() {
    this->kqueueFd = kqueue();
    if (this->kqueueFd < 0) {
        spdlog::error("reactor kqueue failed: {}", strerror(errno));
        return -1;
    }

    // 注册 EVFILT_USER 作为唤醒通道。
    struct kevent kev = {};
    EV_SET(&kev, WAKEUP_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(this->kqueueFd, &kev, 1, nullptr, 0, nullptr) != 0) {
        spdlog::error("reactor register EVFILT_USER failed: {}", strerror(errno));
        ::close(this->kqueueFd);
        this->kqueueFd = -1;
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
    if (this->kqueueFd >= 0) {
        ::close(this->kqueueFd);
        this->kqueueFd = -1;
    }
}

int Reactor::add(int fd, ReactorEvent events, EventHandler handler) {
    {
        std::unique_lock lock(this->handlerMutex);
        this->handlers[fd] = std::move(handler);
    }

    // kqueue 的 READ/WRITE 为独立 filter，按兴趣集分别 EV_ADD。
    struct kevent kev[2];
    int n = 0;
    if (events & ReactorEvent::READ) {
        EV_SET(&kev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    }
    if (events & ReactorEvent::WRITE) {
        EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    }
    if (n > 0 && kevent(this->kqueueFd, kev, n, nullptr, 0, nullptr) != 0) {
        spdlog::error("reactor add fd {} failed: {}", fd, strerror(errno));
        std::unique_lock lock(this->handlerMutex);
        this->handlers.erase(fd);
        return -1;
    }
    this->interests[fd] = events;
    return 0;
}

int Reactor::mod(int fd, ReactorEvent events) {
    // 对比旧兴趣集，对增加的 filter EV_ADD、移除的 filter EV_DELETE，实现整体替换语义。
    ReactorEvent old = ReactorEvent::NONE;
    auto it = this->interests.find(fd);
    if (it != this->interests.end()) {
        old = it->second;
    }

    struct kevent kev[2];
    int n = 0;
    bool oldRead = (old & ReactorEvent::READ);
    bool newRead = (events & ReactorEvent::READ);
    bool oldWrite = (old & ReactorEvent::WRITE);
    bool newWrite = (events & ReactorEvent::WRITE);
    if (newRead && !oldRead) {
        EV_SET(&kev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    } else if (!newRead && oldRead) {
        EV_SET(&kev[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    }
    if (newWrite && !oldWrite) {
        EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    } else if (!newWrite && oldWrite) {
        EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    }
    if (n > 0 && kevent(this->kqueueFd, kev, n, nullptr, 0, nullptr) != 0) {
        // EV_DELETE 对未注册 filter 返回 ENOENT 可忽略。
        if (errno != ENOENT) {
            spdlog::error("reactor mod fd {} failed: {}", fd, strerror(errno));
            return -1;
        }
    }
    this->interests[fd] = events;
    return 0;
}

int Reactor::del(int fd) {
    ReactorEvent old = ReactorEvent::NONE;
    auto it = this->interests.find(fd);
    if (it != this->interests.end()) {
        old = it->second;
    }
    struct kevent kev[2];
    int n = 0;
    if (old & ReactorEvent::READ) {
        EV_SET(&kev[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    }
    if (old & ReactorEvent::WRITE) {
        EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    }
    if (n > 0) {
        kevent(this->kqueueFd, kev, n, nullptr, 0, nullptr);
    }
    this->interests.erase(fd);
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
    if (this->kqueueFd < 0) {
        return;
    }
    struct kevent kev = {};
    EV_SET(&kev, WAKEUP_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    kevent(this->kqueueFd, &kev, 1, nullptr, 0, nullptr);
}

void Reactor::drainWakeup() {
    // EVFILT_USER 配合 EV_CLEAR 为边沿触发，无需额外清空动作。
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
    struct kevent events[MAX_EVENTS];
    while (this->running.load()) {
        int n = kevent(this->kqueueFd, nullptr, 0, events, MAX_EVENTS, nullptr);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            spdlog::error("reactor kevent failed: {}", strerror(errno));
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (events[i].filter == EVFILT_USER && events[i].ident == WAKEUP_IDENT) {
                drainWakeup();
                drainTasks();
                continue;
            }
            int fd = (int)events[i].ident;
            EventHandler handler;
            {
                std::unique_lock lock(this->handlerMutex);
                auto it = this->handlers.find(fd);
                if (it == this->handlers.end()) {
                    continue;
                }
                handler = it->second;
            }
            ReactorEvent ev = ReactorEvent::NONE;
            if (events[i].filter == EVFILT_READ) {
                ev = ev | ReactorEvent::READ;
            }
            if (events[i].filter == EVFILT_WRITE) {
                ev = ev | ReactorEvent::WRITE;
            }
            if (events[i].flags & EV_EOF) {
                ev = ev | ReactorEvent::ERROR;
            }
            handler(ev);
        }
    }
    spdlog::debug("stop thread: netstack reactor");
}

#elif defined(_WIN32) || defined(_WIN64)

// Windows 后端：用 WSAPoll 做就绪通知（无内核兴趣集，每轮按 interests 重建 pollfd 数组），
// 用一对自连环回 UDP socket 做跨线程唤醒（替代 Linux eventfd / macOS EVFILT_USER）。
// 注：与 Linux/macOS 一致，对外仍以 int fd 暴露；SOCKET 句柄在 Windows 上为小整数，转换安全。

static SHORT toWsaEvents(ReactorEvent events) {
    SHORT ev = 0;
    if (events & ReactorEvent::READ) {
        ev |= POLLRDNORM;
    }
    if (events & ReactorEvent::WRITE) {
        ev |= POLLWRNORM;
    }
    return ev;
}

static ReactorEvent fromWsaEvents(SHORT ev) {
    ReactorEvent events = ReactorEvent::NONE;
    if (ev & POLLRDNORM) {
        events = events | ReactorEvent::READ;
    }
    if (ev & POLLWRNORM) {
        events = events | ReactorEvent::WRITE;
    }
    if (ev & (POLLERR | POLLHUP | POLLNVAL)) {
        events = events | ReactorEvent::ERROR;
    }
    return events;
}

Reactor::Reactor() : wakeupRecv(-1), wakeupSend(-1), running(false) {}

Reactor::~Reactor() {
    stop();
}

int Reactor::start() {
    // WSAStartup 引用计数，tun/windows.cc 可能已初始化，这里再调一次安全。
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        spdlog::error("reactor WSAStartup failed: {}", WSAGetLastError());
        return -1;
    }

    SOCKET recvSock = ::socket(AF_INET, SOCK_DGRAM, 0);
    SOCKET sendSock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (recvSock == INVALID_SOCKET || sendSock == INVALID_SOCKET) {
        spdlog::error("reactor wakeup socket failed: {}", WSAGetLastError());
        if (recvSock != INVALID_SOCKET) {
            ::closesocket(recvSock);
        }
        if (sendSock != INVALID_SOCKET) {
            ::closesocket(sendSock);
        }
        WSACleanup();
        return -1;
    }

    // recv 端绑定到 127.0.0.1 随机端口，send 端 connect 到该端口形成自连环回。
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int addrLen = sizeof(addr);
    if (::bind(recvSock, (struct sockaddr *)&addr, addrLen) != 0 ||
        ::getsockname(recvSock, (struct sockaddr *)&addr, &addrLen) != 0 ||
        ::connect(sendSock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        spdlog::error("reactor wakeup loopback setup failed: {}", WSAGetLastError());
        ::closesocket(recvSock);
        ::closesocket(sendSock);
        WSACleanup();
        return -1;
    }

    u_long nonBlocking = 1;
    ::ioctlsocket(recvSock, FIONBIO, &nonBlocking);

    this->wakeupRecv = (int)recvSock;
    this->wakeupSend = (int)sendSock;

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
    if (this->wakeupSend >= 0) {
        ::closesocket((SOCKET)this->wakeupSend);
        this->wakeupSend = -1;
    }
    if (this->wakeupRecv >= 0) {
        ::closesocket((SOCKET)this->wakeupRecv);
        this->wakeupRecv = -1;
    }
    WSACleanup();
}

int Reactor::add(int fd, ReactorEvent events, EventHandler handler) {
    std::unique_lock lock(this->handlerMutex);
    this->handlers[fd] = std::move(handler);
    this->interests[fd] = events;
    return 0;
}

int Reactor::mod(int fd, ReactorEvent events) {
    std::unique_lock lock(this->handlerMutex);
    this->interests[fd] = events;
    return 0;
}

int Reactor::del(int fd) {
    std::unique_lock lock(this->handlerMutex);
    this->interests.erase(fd);
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
    if (this->wakeupSend < 0) {
        return;
    }
    char one = 1;
    ::send((SOCKET)this->wakeupSend, &one, 1, 0);
}

void Reactor::drainWakeup() {
    char buf[256];
    while (::recv((SOCKET)this->wakeupRecv, buf, sizeof(buf), 0) > 0) {
        // 持续读空唤醒 socket，避免残留触发空转。
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
    std::vector<WSAPOLLFD> pfds;
    std::vector<int> fds;
    while (this->running.load()) {
        pfds.clear();
        fds.clear();

        // 唤醒 socket 固定排第一位，只关心可读。
        WSAPOLLFD wake = {};
        wake.fd = (SOCKET)this->wakeupRecv;
        wake.events = POLLRDNORM;
        pfds.push_back(wake);
        fds.push_back(this->wakeupRecv);

        // 快照当前兴趣集，构建 pollfd 数组。
        {
            std::unique_lock lock(this->handlerMutex);
            for (const auto &kv : this->interests) {
                WSAPOLLFD pfd = {};
                pfd.fd = (SOCKET)kv.first;
                pfd.events = toWsaEvents(kv.second);
                pfds.push_back(pfd);
                fds.push_back(kv.first);
            }
        }

        int n = WSAPoll(pfds.data(), (ULONG)pfds.size(), -1);
        if (n < 0) {
            int err = WSAGetLastError();
            if (err == WSAEINTR) {
                continue;
            }
            spdlog::error("reactor WSAPoll failed: {}", err);
            break;
        }
        if (n == 0) {
            continue;
        }

        // 先处理唤醒，再分发 fd 事件。
        if (pfds[0].revents != 0) {
            drainWakeup();
            drainTasks();
        }
        for (size_t i = 1; i < pfds.size(); ++i) {
            if (pfds[i].revents == 0) {
                continue;
            }
            int fd = fds[i];
            EventHandler handler;
            {
                std::unique_lock lock(this->handlerMutex);
                auto it = this->handlers.find(fd);
                if (it == this->handlers.end()) {
                    continue;
                }
                handler = it->second;
            }
            handler(fromWsaEvents(pfds[i].revents));
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
