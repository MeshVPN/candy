// SPDX-License-Identifier: MIT
#include "netstack/reactor.h"
#include <Poco/Net/PollSet.h>
#include <Poco/Net/Socket.h>
#include <Poco/Net/StreamSocketImpl.h>
#include <Poco/Timespan.h>
#include <spdlog/spdlog.h>

namespace candy {

// 非拥有的 SocketImpl：包裹一个由外部（session）创建并负责关闭的落地 fd。
// 关键：析构前把 _sockfd 复位为无效句柄（reset() 不做 close），使基类 ~SocketImpl 的
// close() 成为空操作，避免与 session 的 netClose(fd) 造成双重关闭（fd 可能已被复用）。
class NonOwningSocketImpl : public Poco::Net::StreamSocketImpl {
public:
    explicit NonOwningSocketImpl(int fd) : Poco::Net::StreamSocketImpl(fd) {}
    ~NonOwningSocketImpl() override {
        reset();
    }
};

// 非拥有的 Socket：仅用于把落地 fd 交给 PollSet 监听，不持有 fd 生命周期。
class NonOwningSocket : public Poco::Net::Socket {
public:
    explicit NonOwningSocket(int fd) : Poco::Net::Socket(new NonOwningSocketImpl(fd)) {}
};

// 每个落地 fd 对应一个 IoWatcher：持有非拥有的 Poco::Net::Socket、fd 与事件回调。
// 仅 reactor 线程创建/销毁/访问。
struct IoWatcher {
    int fd;
    Poco::Net::Socket socket;
    Reactor::EventHandler handler;
    ReactorEvent events;
};

static int toPocoMode(ReactorEvent events) {
    int mode = 0;
    if (events & ReactorEvent::READ) {
        mode |= Poco::Net::PollSet::POLL_READ;
    }
    if (events & ReactorEvent::WRITE) {
        mode |= Poco::Net::PollSet::POLL_WRITE;
    }
    return mode;
}

static ReactorEvent fromPocoMode(int mode) {
    ReactorEvent events = ReactorEvent::NONE;
    if (mode & Poco::Net::PollSet::POLL_READ) {
        events = events | ReactorEvent::READ;
    }
    if (mode & Poco::Net::PollSet::POLL_WRITE) {
        events = events | ReactorEvent::WRITE;
    }
    if (mode & Poco::Net::PollSet::POLL_ERROR) {
        events = events | ReactorEvent::FAILURE;
    }
    return events;
}

Reactor::Reactor() : pollSet(nullptr), running(false) {}

Reactor::~Reactor() {
    stop();
}

int Reactor::start() {
    this->pollSet = new Poco::Net::PollSet();
    this->running.store(true);
    this->thread = std::thread([this] { this->loop(); });
    // 在启动线程内确定性记录 reactor 线程 id（早于任何跨线程 add/mod/del 访问），
    // 避免在 loop() 内写、其他线程读 loopThreadId 造成的数据竞争。
    this->loopThreadId = this->thread.get_id();
    return 0;
}

void Reactor::stop() {
    if (!this->running.exchange(false)) {
        if (this->thread.joinable()) {
            this->thread.join();
        }
        return;
    }
    // 唤醒 reactor 线程：poll 立即返回，循环检测到 !running 后退出。
    if (this->pollSet != nullptr) {
        this->pollSet->wakeUp();
    }
    if (this->thread.joinable()) {
        this->thread.join();
    }

    // 线程已退出，安全清理剩余 watcher 与 pollSet（仅本线程访问）。
    for (auto &kv : this->ioWatchers) {
        delete (IoWatcher *)kv.second;
    }
    this->ioWatchers.clear();

    if (this->pollSet != nullptr) {
        delete this->pollSet;
        this->pollSet = nullptr;
    }
}

bool Reactor::onLoopThread() const {
    return std::this_thread::get_id() == this->loopThreadId;
}

// ===================== 对外接口：按线程归属直接应用或投递 =====================

int Reactor::add(int fd, ReactorEvent events, EventHandler handler) {
    if (onLoopThread()) {
        applyAdd(fd, events, std::move(handler));
    } else {
        auto h = std::make_shared<EventHandler>(std::move(handler));
        post([this, fd, events, h] { this->applyAdd(fd, events, std::move(*h)); });
    }
    return 0;
}

int Reactor::mod(int fd, ReactorEvent events) {
    if (onLoopThread()) {
        applyMod(fd, events);
    } else {
        post([this, fd, events] { this->applyMod(fd, events); });
    }
    return 0;
}

int Reactor::del(int fd) {
    if (onLoopThread()) {
        applyDel(fd);
    } else {
        post([this, fd] { this->applyDel(fd); });
    }
    return 0;
}

// ===================== 仅 reactor 线程：实际操作 PollSet =====================

void Reactor::applyAdd(int fd, ReactorEvent events, EventHandler handler) {
    auto it = this->ioWatchers.find(fd);
    if (it != this->ioWatchers.end()) {
        // 已存在：等价于替换 handler 与兴趣集（防御性，正常不应发生）。
        IoWatcher *iw = (IoWatcher *)it->second;
        iw->handler = std::move(handler);
        iw->events = events;
        this->pollSet->update(iw->socket, toPocoMode(events));
        return;
    }
    IoWatcher *iw = new IoWatcher{fd, NonOwningSocket(fd), std::move(handler), events};
    this->pollSet->add(iw->socket, toPocoMode(events));
    this->ioWatchers[fd] = iw;
}

void Reactor::applyMod(int fd, ReactorEvent events) {
    auto it = this->ioWatchers.find(fd);
    if (it == this->ioWatchers.end()) {
        return;
    }
    IoWatcher *iw = (IoWatcher *)it->second;
    if (iw->events == events) {
        return;
    }
    iw->events = events;
    // update 为非累积语义：直接覆盖为新的兴趣集（与原 mod 语义一致）。
    this->pollSet->update(iw->socket, toPocoMode(events));
}

void Reactor::applyDel(int fd) {
    auto it = this->ioWatchers.find(fd);
    if (it == this->ioWatchers.end()) {
        return;
    }
    IoWatcher *iw = (IoWatcher *)it->second;
    this->pollSet->remove(iw->socket);
    this->ioWatchers.erase(it);
    delete iw;
}

// ===================== 跨线程任务投递 =====================

void Reactor::post(Task task) {
    {
        std::unique_lock lock(this->taskMutex);
        this->tasks.push(std::move(task));
    }
    if (this->pollSet != nullptr) {
        this->pollSet->wakeUp();
    }
}

void Reactor::drainTasks() {
    // swap 批量取出，持锁时间极短；锁外执行，允许 task 内再次 post（不会自死锁）。
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
    // poll 超时仅作兜底；跨线程 post/stop 均通过 wakeUp 立即唤醒，不依赖该超时。
    const Poco::Timespan timeout(1, 0); // 1s
    while (this->running.load()) {
        Poco::Net::PollSet::SocketModeMap ready = this->pollSet->poll(timeout);
        for (auto &kv : ready) {
            int fd = (int)kv.first.impl()->sockfd();
            auto it = this->ioWatchers.find(fd);
            if (it == this->ioWatchers.end()) {
                continue;
            }
            IoWatcher *iw = (IoWatcher *)it->second;
            // 关键：先把 handler 拷贝到局部，再调用。handler 内部可能 del(fd) 释放本 IoWatcher，
            // 拷贝后调用可避免 use-after-free；回调返回前不再触碰 iw。
            Reactor::EventHandler handler = iw->handler;
            handler(fromPocoMode(kv.second));
        }
        drainTasks();
    }
    spdlog::debug("stop thread: netstack reactor");
}

} // namespace candy
