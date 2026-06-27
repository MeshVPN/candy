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

// Reactor：基于 epoll 的事件循环（阶段一 Linux 优先；macOS kqueue / Windows IOCP 后续补）。
// 管理所有落地 fd（内核 socket）的可读/可写/错误事件，非阻塞、事件回调驱动。
// 跨线程提交任务用 post()，由 reactor 线程在事件循环中执行（经 eventfd 唤醒）。
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

    int epollFd;
    int wakeupFd;

    std::thread thread;
    std::atomic<bool> running;

    std::mutex handlerMutex;
    std::unordered_map<int, EventHandler> handlers;

    std::mutex taskMutex;
    std::queue<Task> tasks;
};

} // namespace candy

#endif
