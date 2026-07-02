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

// Poco::Net::PollSet 的前向声明，避免在头文件暴露 <Poco/Net/...>（含 winsock）。
namespace Poco {
namespace Net {
class PollSet;
}
} // namespace Poco

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

// Reactor：落地 fd 的事件循环。基于 Poco::Net::PollSet（一份代码全平台：Linux=epoll、
// macOS/BSD=poll、Windows=wepoll，select 兜底），不再手写各平台后端。管理所有落地 fd（内核
// socket）的可读/可写/错误事件，非阻塞、事件回调驱动。
//
// 线程契约：
//   - 事件循环跑在 reactor 自己的线程；PollSet 的 add/update/remove 自带内部锁，可跨线程调用，
//     但为与原有语义一致并保证回调时序，仍统一在 reactor 线程内实际应用。
//   - 跨线程提交任务用 post()：入队后 PollSet::wakeUp 唤醒 reactor 线程，由其在下一轮 poll 后
//     抽干执行。wakeUp 通过 eventfd(Linux)/pipe(BSD) 实现，线程安全且不丢唤醒。
//   - add/mod/del 对 PollSet 的实际操作必须在 reactor 线程执行：若调用方已在 reactor 线程
//     （事件回调内）则直接应用；否则经 post 投递到 reactor 线程应用，保证 fd->handler 表一致。
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
    // 以下三个 apply* 只在 reactor 线程执行，直接操作 PollSet 与 ioWatchers 表。
    void applyAdd(int fd, ReactorEvent events, EventHandler handler);
    void applyMod(int fd, ReactorEvent events);
    void applyDel(int fd);

    Poco::Net::PollSet *pollSet;
    // fd -> IoWatcher*（IoWatcher 定义于 .cc，内含非拥有的 Poco::Net::Socket 与事件回调）。
    // 仅 reactor 线程访问，无需加锁。
    std::unordered_map<int, void *> ioWatchers;

    std::thread thread;
    std::atomic<bool> running;
    std::thread::id loopThreadId;

    std::mutex taskMutex;
    std::queue<Task> tasks;
};

} // namespace candy

#endif
