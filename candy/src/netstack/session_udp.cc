// SPDX-License-Identifier: MIT
#include "netstack/session_udp.h"
#include "netstack/netstack.h"
#include "netstack/sockcompat.h"
#include <cstring>
#include <spdlog/spdlog.h>

#include "lwip/pbuf.h"
#include "lwip/udp.h"

#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#elif defined(_WIN32) || defined(_WIN64)
#include <ws2tcpip.h>
#endif

namespace candy {

SessionUdp::SessionUdp(NetStack *stack, struct udp_pcb *pcb, IP4 origSrc, uint16_t origSrcPort)
    : Session(stack), pcb(pcb), fd(-1), origSrc(origSrc), origSrcPort(origSrcPort),
      lastActiveTs(std::chrono::steady_clock::now()) {
    // 源二元组 key：源IP:源端口（全锥形下一源一会话，目的 per-packet 不入 key）
    this->sessionKey.assign((const char *)&origSrc, sizeof(uint32_t));
    this->sessionKey.append((const char *)&origSrcPort, sizeof(origSrcPort));
}

SessionUdp::~SessionUdp() {
    if (this->fd >= 0) {
        netClose(this->fd);
        this->fd = -1;
    }
}

int SessionUdp::start() {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
    this->self = shared_from_this();

    // 落地拨号：内核 socket 直连（DirectOutbound）。全锥形——unconnected + 固定出口端口，
    // 后续用 sendto 发往任意目的、recvfrom 收任意对端回包。出口源 IP 由内核填为网关出口 IP。
    Outbound &outbound = this->stack->getOutbound();
    this->fd = outbound.dialUdp();
    if (this->fd < 0) {
        return -1;
    }

    spdlog::debug("session udp src: {}:{}", this->origSrc.toString(), this->origSrcPort);

    auto holder = shared_from_this();
    int fd = this->fd;
    this->stack->getReactor().add(fd, ReactorEvent::READ, [holder](ReactorEvent ev) { holder->onFdEvent((uint32_t)ev); });

    // 在 lwIP 克隆出的 npcb 上注册每流 recv：本流后续数据报都回调到 recvTrampoline。
    // recv_arg 用裸指针 this：会话存活期间被 udpSessions 持有 + self 自持，指针有效。
    udp_recv(this->pcb, recvTrampoline, this);
    return 0;
#else
    return -1;
#endif
}

// ===================== NetStack 线程 =====================

void SessionUdp::recvTrampoline(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    SessionUdp *self = (SessionUdp *)arg;
    self->onPcbRecv(p, addr, port);
}

void SessionUdp::onPcbRecv(struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    // 本流后续数据报：addr/port=源端(dev1)。目的由 lwIP 在回调前覆写进 pcb->local_*，
    // 必须此刻同步读取（下一入站包会再次覆写），随数据一并按值投递到 reactor。
    if (p == nullptr) {
        return;
    }
    uint32_t dstIpBe;
    std::memcpy(&dstIpBe, &ip_2_ip4(&this->pcb->local_ip)->addr, sizeof(uint32_t));
    uint16_t dstPortHost = this->pcb->local_port; // lwIP 端口为主机序
    std::string data;
    data.resize(p->tot_len);
    pbuf_copy_partial(p, data.data(), p->tot_len, 0);
    pbuf_free(p);
    sendToLanding(dstIpBe, dstPortHost, std::move(data));
}

void SessionUdp::sendToLanding(uint32_t dstIpBe, uint16_t dstPortHost, std::string data) {
    // 仅 NetStack 线程调用：刷新活跃时间，把数据报连同目的投递到 reactor 线程用 sendto 发送。
    this->lastActiveTs = std::chrono::steady_clock::now();
    auto holder = shared_from_this();
    this->stack->getReactor().post([holder, dstIpBe, dstPortHost, data = std::move(data)]() mutable {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
        if (holder->closing.load()) {
            return;
        }
        if (holder->fd < 0) {
            return;
        }
        long n = netSendTo(holder->fd, data.data(), data.size(), dstIpBe, dstPortHost);
        if (n < 0 && !netWouldBlock(netLastError())) {
            spdlog::debug("session udp sendto failed: {}", netErrStr(netLastError()));
        }
#endif
    });
}

void SessionUdp::replyToLwip(uint32_t peerIpBe, uint16_t peerPortHost, std::string data) {
    // 仅 NetStack 线程调用：把落地回包以 (实际对端 -> origSrc:origSrcPort) 重新注入 lwIP，
    // 由 netif->output 产生内层 IP 包、再经 NetStack::output 封 IPIP 回源端。
    // 全锥形：回注源用「实际对端」(可为 P2P 对端/STUN)，而非固定落地目的，支撑打洞回包。
    if (this->pcb == nullptr) {
        return;
    }
    this->lastActiveTs = std::chrono::steady_clock::now();

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)data.size(), PBUF_RAM);
    if (p == nullptr) {
        spdlog::warn("session udp reply pbuf_alloc failed");
        return;
    }
    pbuf_take(p, data.data(), (u16_t)data.size());

    // udp_sendto_if_src 的 UDP 源端口取自 pcb->local_port（非参数），故注入前先写为对端端口。
    // pretend pcb 的 local_* 不参与入站匹配(只认 remote_*)，且下一正向包会被 lwIP 再次覆写，安全。
    this->pcb->local_port = peerPortHost;

    // 源地址=实际对端(peerIp:peerPort)；目的=源端(origSrc:origSrcPort)。
    ip_addr_t src;
    ip_addr_t dst;
    ip_addr_set_ip4_u32(&src, peerIpBe);
    ip_addr_set_ip4_u32(&dst, uint32_t(this->origSrc));
    err_t e = udp_sendto_if_src(this->pcb, p, &dst, this->origSrcPort, &this->stack->getNetif(), &src);
    if (e != ERR_OK) {
        spdlog::debug("session udp udp_sendto_if_src failed: {}", (int)e);
    }
    pbuf_free(p);
}

void SessionUdp::closeFromStack() {
    if (this->pcb != nullptr) {
        udp_recv(this->pcb, nullptr, nullptr);
        udp_remove(this->pcb);
        this->pcb = nullptr;
    }
    NetStack *stack = this->stack;
    std::string k = this->sessionKey;
    auto holder = shared_from_this();
    stack->getReactor().post([holder] { holder->closeFromReactor(); });
    stack->removeUdpSession(k);
}

void SessionUdp::shutdownFromStack() {
    // 栈拆除路径：NetStack 线程内直接移除 pcb；fd 交由 reactor 关闭。
    if (this->pcb != nullptr) {
        udp_recv(this->pcb, nullptr, nullptr);
        udp_remove(this->pcb);
        this->pcb = nullptr;
    }
    if (!this->closing.exchange(true)) {
        auto holder = shared_from_this();
        this->stack->getReactor().post([holder] {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
            if (holder->fd >= 0) {
                holder->stack->getReactor().del(holder->fd);
                netClose(holder->fd);
                holder->fd = -1;
            }
#endif
            holder->self.reset();
        });
    }
}

// ===================== Reactor 线程 =====================

void SessionUdp::onFdEvent(uint32_t events) {
    ReactorEvent ev = (ReactorEvent)events;
    if (ev & ReactorEvent::FAILURE) {
        closeFromReactor();
        return;
    }
    if (ev & ReactorEvent::READ) {
        onFdReadable();
    }
}

void SessionUdp::onFdReadable() {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
    char buf[65536];
    while (true) {
        // 全锥形：unconnected socket 用 recvfrom 收任意对端回包，同时取回真实对端地址/端口，
        // 连同数据投递到 NetStack 线程，由 replyToLwip 以该对端为源注入回 dev1（支撑打洞回包）。
        uint32_t peerIpBe;
        uint16_t peerPortHost;
        long n = netRecvFrom(this->fd, buf, sizeof(buf), &peerIpBe, &peerPortHost);
        if (n > 0) {
            std::string data(buf, n);
            auto holder = shared_from_this();
            this->stack->postToStack([holder, peerIpBe, peerPortHost, data = std::move(data)]() mutable {
                holder->replyToLwip(peerIpBe, peerPortHost, std::move(data));
            });
            continue;
        }
        if (n == 0) {
            return;
        }
        if (netWouldBlock(netLastError())) {
            return;
        }
        // 落地 socket 错误：unconnected 下罕见（对称型 connect socket 才会经 ICMP 收到
        // ECONNREFUSED），保留关闭分支兜底，空闲回收主要交由 reapIdleUdpSessions。
        closeFromReactor();
        return;
    }
#endif
}

void SessionUdp::closeFromReactor() {
    if (this->closing.exchange(true)) {
        return;
    }
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
    if (this->fd >= 0) {
        this->stack->getReactor().del(this->fd);
        netClose(this->fd);
        this->fd = -1;
    }
#endif
    if (this->pcb != nullptr) {
        auto holder = shared_from_this();
        this->stack->postToStack([holder] { holder->closeFromStack(); });
    }
    auto keep = this->self;
    this->self.reset();
}

} // namespace candy
