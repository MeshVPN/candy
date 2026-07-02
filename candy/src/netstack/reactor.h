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

// libev 的不透明类型前向声明，避免在头文件暴露 <ev.h>。
struct ev_loop;
struct ev_io;
struct ev_async;

namespace candy {

enum class ReactorEvent : uint32_t {
    NONE = 0,
    READ = 1 << 0,
    WRITE = 1 << 1,
    FAILURE = 1 << 2,
};

inline ReactorEvent operator|(ReactorEvent a, ReactorEvent b) {
    return static_cast<ReactorEvent>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool operator&(ReactorEvent a, ReactorEvent b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

// Reactor：落地 fd 的事件循环。基于 libev（一份代码全平台：Linux=epoll、macOS/iOS=kqueue、
// Android=epoll、其余=poll/select 兜底），不再手写各平台后端。管理所有落地 fd（内核 socket）
// 的可读/可写/错误事件，非阻塞、事件回调驱动。
//
// 线程契约：
//   - 事件循环跑在 reactor 自己的线程；libev 除 ev_async_send 外均非线程安全。
//   - 跨线程提交任务用 post()：入队后 ev_async_send 唤醒 reactor 线程，由其在 async 回调里抽干执行。
//   - add/mod/del 对 libev watcher 的实际操作必须在 reactor 线程执行：若调用方已在 reactor 线程
//     （事件回调内）则直接应用；否则经 post 投递到 reactor 线程应用，保证线程安全。
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
    void drainTasks();

    bool onLoopThread() const;
    // 以下三个 apply* 只在 reactor 线程执行，直接操作 libev watcher 与 ioWatchers 表。
    void applyAdd(int fd, ReactorEvent events, EventHandler handler);
    void applyMod(int fd, ReactorEvent events);
    void applyDel(int fd);

    // libev 回调跳板（静态成员，C 兼容签名）。
    static void ioCallback(struct ev_loop *loop, struct ev_io *w, int revents);
    static void asyncCallback(struct ev_loop *loop, struct ev_async *w, int revents);

    struct ev_loop *evLoop;
    void *asyncWatcher; // ev_async*，堆分配
    // fd -> IoWatcher*（IoWatcher 定义于 .cc）。仅 reactor 线程访问，无需加锁。
    std::unordered_map<int, void *> ioWatchers;

    std::thread thread;
    std::atomic<bool> running;
    std::thread::id loopThreadId;

    std::mutex taskMutex;
    std::queue<Task> tasks;
};

} // namespace candy

#endif
