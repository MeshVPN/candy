// SPDX-License-Identifier: MIT
#include "netstack/session_tcp.h"
#include "netstack/netstack.h"
#include <algorithm>
#include <cstring>
#include <spdlog/spdlog.h>

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace candy {

// fd -> lwIP 方向背压水位：积压达到 HIGH_WATER 暂停从落地 fd 读取，回落到 LOW_WATER 恢复。
// 取值需高于 lwIP 单条 TCP 发送缓冲（TCP_SND_BUF≈46KB），留出在途 + 任务队列余量。
static constexpr size_t HIGH_WATER = 256 * 1024;
static constexpr size_t LOW_WATER = 64 * 1024;

SessionTcp::SessionTcp(NetStack *stack, struct tcp_pcb *pcb)
    : Session(stack), pcb(pcb), fd(-1), connected(false), origDstPort(0), wantWrite(false), fdEof(false) {
    std::memcpy(&this->origDst, &ip_2_ip4(&pcb->local_ip)->addr, sizeof(uint32_t));
    this->origDstPort = pcb->local_port;
    std::memcpy(&this->origSrc, &ip_2_ip4(&pcb->remote_ip)->addr, sizeof(uint32_t));
}

SessionTcp::~SessionTcp() {
    if (this->fd >= 0) {
        ::close(this->fd);
        this->fd = -1;
    }
}

int SessionTcp::start() {
    this->self = shared_from_this();

    tcp_arg(this->pcb, this);
    tcp_recv(this->pcb, recvTrampoline);
    tcp_sent(this->pcb, sentTrampoline);
    tcp_err(this->pcb, errTrampoline);

#if defined(__linux__) || defined(__APPLE__)
    this->fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (this->fd < 0) {
        spdlog::warn("session tcp socket failed: {}", strerror(errno));
        return -1;
    }
    int flags = ::fcntl(this->fd, F_GETFL, 0);
    ::fcntl(this->fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(this->origDstPort);
    dst.sin_addr.s_addr = uint32_t(this->origDst);

    int ret = ::connect(this->fd, (struct sockaddr *)&dst, sizeof(dst));
    if (ret != 0 && errno != EINPROGRESS) {
        spdlog::warn("session tcp connect {}:{} failed: {}", this->origDst.toString(), this->origDstPort,
                     strerror(errno));
        ::close(this->fd);
        this->fd = -1;
        return -1;
    }

    spdlog::debug("session tcp: {} -> {}:{}", this->origSrc.toString(), this->origDst.toString(), this->origDstPort);

    auto holder = shared_from_this();
    int fd = this->fd;
    this->stack->getReactor().add(fd, ReactorEvent::WRITE, [holder](ReactorEvent ev) { holder->onFdEvent((uint32_t)ev); });
    return 0;
#else
    return -1;
#endif
}

// ===================== NetStack 线程 =====================

err_t SessionTcp::recvTrampoline(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    SessionTcp *self = (SessionTcp *)arg;
    return self->onRecv(pcb, p, err);
}

err_t SessionTcp::sentTrampoline(void *arg, struct tcp_pcb *pcb, u16_t len) {
    SessionTcp *self = (SessionTcp *)arg;
    return self->onSent(pcb, len);
}

void SessionTcp::errTrampoline(void *arg, err_t err) {
    SessionTcp *self = (SessionTcp *)arg;
    self->onErr(err);
}

err_t SessionTcp::onRecv(struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (p == nullptr || err != ERR_OK) {
        // 对端(lwIP 侧)关闭：通知 reactor 关闭落地 fd
        if (p != nullptr) {
            pbuf_free(p);
        }
        closeFromStack();
        return ERR_OK;
    }

    std::string data;
    data.resize(p->tot_len);
    pbuf_copy_partial(p, data.data(), p->tot_len, 0);
    pbuf_free(p);

    {
        std::unique_lock lock(this->bufMutex);
        this->forwardBuf.append(data);
    }
    // 注意：不在此处 tcp_recved！接收窗口的推进延迟到数据真正写进落地 fd 之后
    // （flushForwardLocked 累加 pendingRecved -> NetStack 线程 ackRecvedToLwip）。
    // 否则会向源端谎报"已收下"，落地端写得慢时 forwardBuf 无界膨胀直至 OOM。

    auto holder = shared_from_this();
    this->stack->getReactor().post([holder] { holder->flushForwardLocked(); });
    return ERR_OK;
}

err_t SessionTcp::onSent(struct tcp_pcb *pcb, u16_t len) {
    // sndbuf 释放了空间，继续把待发缓冲灌进 lwIP。
    pumpToLwip();
    return ERR_OK;
}

void SessionTcp::onErr(err_t err) {
    // pcb 已被 lwIP 释放
    this->pcb = nullptr;
    closeFromReactor();
}

void SessionTcp::writeToLwip(const std::string &data) {
    if (this->pcb == nullptr) {
        return;
    }
    this->backwardBuf.append(data);
    pumpToLwip();
}

void SessionTcp::pumpToLwip() {
    // 仅在 NetStack 线程调用。把 backwardBuf 尽量写入 lwIP 发送缓冲，
    // 写不下的保留，等 onSent 回调再续；并据缓冲水位对落地 fd 读做背压。
    if (this->pcb == nullptr) {
        return;
    }
    size_t offset = 0;
    bool written = false;
    while (offset < this->backwardBuf.size()) {
        u16_t space = tcp_sndbuf(this->pcb);
        if (space == 0) {
            break;
        }
        u16_t chunk = (u16_t)std::min((size_t)space, this->backwardBuf.size() - offset);
        err_t e = tcp_write(this->pcb, this->backwardBuf.data() + offset, chunk, TCP_WRITE_FLAG_COPY);
        if (e == ERR_MEM) {
            break;
        }
        if (e != ERR_OK) {
            return;
        }
        offset += chunk;
        written = true;
    }
    if (offset > 0) {
        this->backwardBuf.erase(0, offset);
        // 这部分已真正进入 lwIP 发送缓冲，从积压中扣除。
        size_t prev = this->backwardBytes.fetch_sub(offset);
        (void)prev;
    }
    if (written) {
        tcp_output(this->pcb);
    }
    updateReadInterest();

    // 落地端已 EOF 且待发缓冲已排空：此时才能安全关闭 lwIP 侧，
    // 否则会丢掉尾部数据（之前 ~80% 处 connection closed prematurely 的根因）。
    if (this->fdEof && this->backwardBuf.empty()) {
        closeFromStack();
    }
}

void SessionTcp::onFdEofFromStack() {
    this->fdEof = true;
    // 尝试把残留数据灌完；若已空则 pumpToLwip 内部会触发 closeFromStack。
    pumpToLwip();
    if (this->backwardBuf.empty()) {
        closeFromStack();
    }
}

void SessionTcp::ackRecvedToLwip() {
    // 仅 NetStack 线程调用。把已写进落地 fd 的字节向 lwIP 确认，推进接收窗口。
    // 多个 flush 的确认会被合并在一次调用里消费（exchange 取走累计值）。
    if (this->pcb == nullptr) {
        return;
    }
    size_t acked = this->pendingRecved.exchange(0);
    if (acked == 0) {
        return;
    }
    // tcp_recved 的 len 是 u16_t，分批确认避免溢出。
    while (acked > 0) {
        u16_t chunk = (u16_t)std::min(acked, (size_t)0xFFFF);
        tcp_recved(this->pcb, chunk);
        acked -= chunk;
    }
}

void SessionTcp::updateReadInterest() {
    // 背压恢复（NetStack 线程）：积压回落到低水位且当前处于暂停态时，恢复从落地 fd 读取。
    // 暂停动作在 Reactor 线程 onFdReadable 内同步完成（读到高水位即停），二者形成闭环：
    //   Reactor 读出 -> backwardBytes += -> 到高水位停读
    //   NetStack pump 进 lwIP -> backwardBytes -= -> 到低水位恢复读
    // 如此 fd 读取速率被 lwIP 发送+ACK 回收节奏严格约束，不再淹没单线程 NetStack。
    if (this->readPaused.load() && this->backwardBytes.load() <= LOW_WATER) {
        this->readPaused.store(false);
        int fd = this->fd;
        auto holder = shared_from_this();
        this->stack->getReactor().post([holder, fd] {
            if (fd >= 0 && !holder->closing.load()) {
                holder->stack->getReactor().mod(fd, ReactorEvent::READ);
            }
        });
    }
}

void SessionTcp::closeFromStack() {
    if (this->pcb != nullptr) {
        tcp_arg(this->pcb, nullptr);
        tcp_recv(this->pcb, nullptr);
        tcp_sent(this->pcb, nullptr);
        tcp_err(this->pcb, nullptr);
        if (tcp_close(this->pcb) != ERR_OK) {
            tcp_abort(this->pcb);
        }
    }
    struct tcp_pcb *closedPcb = this->pcb;
    this->pcb = nullptr;

    NetStack *stack = this->stack;
    auto holder = shared_from_this();
    stack->getReactor().post([holder] { holder->closeFromReactor(); });
    if (closedPcb != nullptr) {
        stack->removeSession(closedPcb);
    }
}

void SessionTcp::shutdownFromStack() {
    // 栈拆除路径：在 NetStack 线程内直接 abort pcb（此时 netif 即将移除，
    // 不能再依赖正常 tcp_close 的四次挥手）。fd 交由 reactor 关闭。
    if (this->pcb != nullptr) {
        tcp_arg(this->pcb, nullptr);
        tcp_recv(this->pcb, nullptr);
        tcp_sent(this->pcb, nullptr);
        tcp_err(this->pcb, nullptr);
        tcp_abort(this->pcb);
        this->pcb = nullptr;
    }
    if (!this->closing.exchange(true)) {
        auto holder = shared_from_this();
        this->stack->getReactor().post([holder] {
#if defined(__linux__) || defined(__APPLE__)
            if (holder->fd >= 0) {
                holder->stack->getReactor().del(holder->fd);
                ::close(holder->fd);
                holder->fd = -1;
            }
#endif
            holder->self.reset();
        });
    }
}

// ===================== Reactor 线程 =====================

void SessionTcp::onFdEvent(uint32_t events) {
    ReactorEvent ev = (ReactorEvent)events;
    if (!this->connected) {
        if (ev & ReactorEvent::ERROR) {
            closeFromReactor();
            return;
        }
        if (ev & ReactorEvent::WRITE) {
            onConnected();
        }
        return;
    }
    // 已连接：优先把可读数据/EOF 读干净（read 返回 0 走优雅排空，返回 -1 走关闭），
    // 避免 ERROR/HUP 与可读数据同时到达时抢先关闭而丢失尾部数据。
    if (ev & (ReactorEvent::READ | ReactorEvent::ERROR)) {
        onFdReadable();
    }
    if (ev & ReactorEvent::WRITE) {
        onFdWritable();
    }
}

void SessionTcp::onConnected() {
#if defined(__linux__) || defined(__APPLE__)
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(this->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        spdlog::warn("session tcp connect result error: {}", strerror(err ? err : errno));
        closeFromReactor();
        return;
    }
    this->connected = true;
    this->stack->getReactor().mod(this->fd, ReactorEvent::READ);
    flushForwardLocked();
#endif
}

void SessionTcp::flushForwardLocked() {
#if defined(__linux__) || defined(__APPLE__)
    if (this->fd < 0 || !this->connected) {
        return;
    }
    size_t acked = 0;
    {
        std::unique_lock lock(this->bufMutex);
        while (!this->forwardBuf.empty()) {
            ssize_t n = ::write(this->fd, this->forwardBuf.data(), this->forwardBuf.size());
            if (n > 0) {
                this->forwardBuf.erase(0, n);
                acked += (size_t)n;
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                this->wantWrite = true;
                this->stack->getReactor().mod(this->fd, ReactorEvent::READ | ReactorEvent::WRITE);
                break;
            }
            lock.unlock();
            closeFromReactor();
            return;
        }
        if (this->forwardBuf.empty() && this->wantWrite) {
            this->wantWrite = false;
            this->stack->getReactor().mod(this->fd, ReactorEvent::READ);
        }
    }
    // 已落地的字节交由 NetStack 线程向 lwIP 确认（推进接收窗口）。
    // 这才是真正的"收到"，从而把源端发送速率约束在落地 fd 的消费能力之内。
    if (acked > 0) {
        this->pendingRecved.fetch_add(acked);
        auto holder = shared_from_this();
        this->stack->postToStack([holder] { holder->ackRecvedToLwip(); });
    }
#endif
}

void SessionTcp::onFdWritable() {
    flushForwardLocked();
}

void SessionTcp::onFdReadable() {
#if defined(__linux__) || defined(__APPLE__)
    char buf[65536];
    while (true) {
        // 同步背压：积压超过高水位立即停读，避免单线程 NetStack 被 writeToLwip 任务淹没。
        // 恢复由 NetStack 线程在 pumpToLwip（onSent 驱动）排空到低水位后触发。
        if (this->backwardBytes.load() >= HIGH_WATER) {
            this->readPaused.store(true);
            this->stack->getReactor().mod(this->fd, ReactorEvent::NONE);
            return;
        }
        ssize_t n = ::read(this->fd, buf, sizeof(buf));
        if (n > 0) {
            this->backwardBytes.fetch_add((size_t)n);
            std::string data(buf, n);
            auto holder = shared_from_this();
            this->stack->postToStack([holder, data = std::move(data)]() mutable { holder->writeToLwip(data); });
            continue;
        }
        if (n == 0) {
            // 落地端关闭：等 backwardBuf 灌完再关 lwIP 侧，避免尾部数据丢失。
            auto holder = shared_from_this();
            this->stack->postToStack([holder] { holder->onFdEofFromStack(); });
            this->stack->getReactor().del(this->fd);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        closeFromReactor();
        return;
    }
#endif
}

void SessionTcp::closeFromReactor() {
    if (this->closing.exchange(true)) {
        return;
    }
#if defined(__linux__) || defined(__APPLE__)
    if (this->fd >= 0) {
        this->stack->getReactor().del(this->fd);
        ::close(this->fd);
        this->fd = -1;
    }
#endif
    if (this->pcb != nullptr) {
        auto holder = shared_from_this();
        this->stack->postToStack([holder] { holder->closeFromStack(); });
    }
    // 释放自持引用（最后一个引用消失时析构）
    auto keep = this->self;
    this->self.reset();
}

} // namespace candy
