// SPDX-License-Identifier: MIT
#ifndef CANDY_NETSTACK_REACTOR_H
#define CANDY_NETSTACK_REACTOR_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

namespace candy {

enum class ReactorEvent : uint32_t {
    NONE = 0,
    READ = 1 << 0,
    WRITE = 1 << 1,
    ERROR = 1 << 2,
};

inline ReactorEvent operator|(ReactorEvent a, ReactorEvent b) {
    return static_cast<ReactorEvent>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool operator&(ReactorEvent a, ReactorEvent b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

// Reactor：落地 fd 的事件循环。Linux 用 epoll + eventfd，macOS 用 kqueue + EVFILT_USER，
// Windows 用 WSAPoll + 自连环回 UDP 唤醒 socket。管理所有落地 fd（内核 socket）的可读/可写/错误事件，
// 非阻塞、事件回调驱动。跨线程提交任务用 post()，由 reactor 线程在事件循环中执行（经唤醒机制）。
class Reactor {
public:
    using EventHandler = std::function<void(ReactorEvent)>;
    using Task = std::function<void()>;

    Reactor();
    ~Reactor();

    int start();
    void stop();

    int add(int fd, ReactorEvent events, EventHandler handler);
    int mod(int fd, ReactorEvent events);
    int del(int fd);

    void post(Task task);

private:
    void loop();
    void wakeup();
    void drainTasks();
    void drainWakeup();

#if defined(__linux__)
    int epollFd;
    int wakeupFd;
#elif defined(__APPLE__)
    int kqueueFd;
    // kqueue 的 READ/WRITE 为独立 filter，需记录每个 fd 当前兴趣集以做增量增删。
    std::unordered_map<int, ReactorEvent> interests;
#elif defined(_WIN32) || defined(_WIN64)
    // WSAPoll 无内核兴趣集，需在用户态维护 fd->兴趣集，每轮重建 pollfd 数组。
    std::unordered_map<int, ReactorEvent> interests;
    // 自连环回 UDP socket：wakeupRecv 注册进 poll，wakeup() 时往 wakeupSend 发一字节触发可读。
    int wakeupRecv;
    int wakeupSend;
#else
    int epollFd;
    int wakeupFd;
#endif

    std::thread thread;
    std::atomic<bool> running;

    std::mutex handlerMutex;
    std::unordered_map<int, EventHandler> handlers;

    std::mutex taskMutex;
    std::queue<Task> tasks;
};

} // namespace candy

#endif
