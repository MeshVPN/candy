// SPDX-License-Identifier: MIT
#include "netstack/reactor.h"
#include <ev.h>
#include <spdlog/spdlog.h>

namespace candy {

// 每个落地 fd 对应一个 IoWatcher：内嵌 libev 的 ev_io，并携带 fd、所属 Reactor 与事件回调。
// 仅 reactor 线程创建/销毁/访问。io.data 指回本结构以便在静态回调里取回上下文。
struct IoWatcher {
    ev_io io;
    int fd;
    Reactor *reactor;
    Reactor::EventHandler handler;
    ReactorEvent events;
};

static int toEvEvents(ReactorEvent events) {
    int ev = 0;
    if (events & ReactorEvent::READ) {
        ev |= EV_READ;
    }
    if (events & ReactorEvent::WRITE) {
        ev |= EV_WRITE;
    }
    return ev;
}

static ReactorEvent fromEvEvents(int revents) {
    ReactorEvent events = ReactorEvent::NONE;
    if (revents & EV_READ) {
        events = events | ReactorEvent::READ;
    }
    if (revents & EV_WRITE) {
        events = events | ReactorEvent::WRITE;
    }
    if (revents & EV_ERROR) {
        events = events | ReactorEvent::ERROR;
    }
    return events;
}

Reactor::Reactor() : evLoop(nullptr), asyncWatcher(nullptr), running(false) {}

Reactor::~Reactor() {
    stop();
}

// 按平台显式指定 libev 后端，与 cmake/libev.cmake 编入的 EV_USE_* 严格对齐。
// 不能用 EVFLAG_AUTO：AUTO 会走 ev_recommended_backends()，而该函数在 __APPLE__
// 下硬编码剔除 KQUEUE 与 POLL（ev.c 注释 "horribly broken"），只留 SELECT；
// 但我们为缩小风险面把 EV_USE_SELECT 关了，导致 mac 上推荐后端集合为空、
// ev_loop_new 返回 null（reactor ev_loop_new failed）。
// reactor 只用后端监听 TCP/UDP socket（utun fd 由 tun 线程单独读写，不进 reactor），
// 普通 socket 上 kqueue 可靠，故这里显式指定 kqueue 绕过推荐过滤。
static unsigned int reactorBackendFlags() {
#if defined(__APPLE__)
    return EVBACKEND_KQUEUE; // 与 EV_USE_KQUEUE=1 对齐
#elif defined(__linux__) || defined(__ANDROID__)
    return EVBACKEND_EPOLL;  // 与 EV_USE_EPOLL=1 对齐（musl 下也走 epoll）
#elif defined(_WIN32)
    return EVBACKEND_SELECT; // 与 EV_USE_SELECT=1 / EV_SELECT_IS_WINSOCKET 对齐
#else
    return EVFLAG_AUTO;
#endif
}

int Reactor::start() {
    // 独立事件循环（不使用默认 loop，避免与进程内其他 libev 使用者冲突）。
    this->evLoop = ev_loop_new(reactorBackendFlags());
    if (this->evLoop == nullptr) {
        spdlog::error("reactor ev_loop_new failed");
        return -1;
    }

    // ev_async：唯一可跨线程安全触发的 watcher，用作 post() 的唤醒通道。
    ev_async *async = new ev_async();
    ev_async_init(async, &Reactor::asyncCallback);
    async->data = this;
    ev_async_start(this->evLoop, async);
    this->asyncWatcher = async;

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
    // 唤醒 reactor 线程，async 回调里检测到 !running 会 ev_break 退出 ev_run。
    if (this->asyncWatcher != nullptr) {
        ev_async_send(this->evLoop, (ev_async *)this->asyncWatcher);
    }
    if (this->thread.joinable()) {
        this->thread.join();
    }

    // 线程已退出，安全清理剩余 watcher 与 loop（仅本线程访问）。
    for (auto &kv : this->ioWatchers) {
        IoWatcher *iw = (IoWatcher *)kv.second;
        ev_io_stop(this->evLoop, &iw->io);
        delete iw;
    }
    this->ioWatchers.clear();

    if (this->asyncWatcher != nullptr) {
        ev_async_stop(this->evLoop, (ev_async *)this->asyncWatcher);
        delete (ev_async *)this->asyncWatcher;
        this->asyncWatcher = nullptr;
    }
    if (this->evLoop != nullptr) {
        ev_loop_destroy(this->evLoop);
        this->evLoop = nullptr;
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

// ===================== 仅 reactor 线程：实际操作 libev watcher =====================

void Reactor::applyAdd(int fd, ReactorEvent events, EventHandler handler) {
    auto it = this->ioWatchers.find(fd);
    if (it != this->ioWatchers.end()) {
        // 已存在：等价于替换 handler 与兴趣集（防御性，正常不应发生）。
        IoWatcher *iw = (IoWatcher *)it->second;
        ev_io_stop(this->evLoop, &iw->io);
        iw->handler = std::move(handler);
        iw->events = events;
        ev_io_set(&iw->io, fd, toEvEvents(events));
        ev_io_start(this->evLoop, &iw->io);
        return;
    }
    IoWatcher *iw = new IoWatcher();
    iw->fd = fd;
    iw->reactor = this;
    iw->handler = std::move(handler);
    iw->events = events;
    iw->io.data = iw;
    ev_io_init(&iw->io, &Reactor::ioCallback, fd, toEvEvents(events));
    ev_io_start(this->evLoop, &iw->io);
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
    // libev 不允许修改运行中的 watcher，需 stop -> set -> start。
    ev_io_stop(this->evLoop, &iw->io);
    iw->events = events;
    ev_io_set(&iw->io, fd, toEvEvents(events));
    ev_io_start(this->evLoop, &iw->io);
}

void Reactor::applyDel(int fd) {
    auto it = this->ioWatchers.find(fd);
    if (it == this->ioWatchers.end()) {
        return;
    }
    IoWatcher *iw = (IoWatcher *)it->second;
    ev_io_stop(this->evLoop, &iw->io);
    this->ioWatchers.erase(it);
    delete iw;
}

// ===================== 跨线程任务投递 =====================

void Reactor::post(Task task) {
    {
        std::unique_lock lock(this->taskMutex);
        this->tasks.push(std::move(task));
    }
    if (this->asyncWatcher != nullptr) {
        ev_async_send(this->evLoop, (ev_async *)this->asyncWatcher);
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

// ===================== libev 回调跳板 =====================

void Reactor::ioCallback(struct ev_loop *, struct ev_io *w, int revents) {
    IoWatcher *iw = (IoWatcher *)w->data;
    // 关键：先把 handler 拷贝到局部，再调用。handler 内部可能 del(fd) 释放本 IoWatcher，
    // 拷贝后调用可避免 use-after-free；回调返回前不再触碰 iw。
    Reactor::EventHandler handler = iw->handler;
    handler(fromEvEvents(revents));
}

void Reactor::asyncCallback(struct ev_loop *loop, struct ev_async *w, int) {
    Reactor *self = (Reactor *)w->data;
    self->drainTasks();
    if (!self->running.load()) {
        ev_break(loop, EVBREAK_ALL);
    }
}

void Reactor::loop() {
    spdlog::debug("start thread: netstack reactor");
    ev_run(this->evLoop, 0);
    spdlog::debug("stop thread: netstack reactor");
}

} // namespace candy
