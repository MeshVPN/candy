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

SessionUdp::SessionUdp(NetStack *stack, struct udp_pcb *pcb, IP4 origSrc, uint16_t origSrcPort, IP4 origDst,
                       uint16_t origDstPort)
    : Session(stack), pcb(pcb), fd(-1), origSrc(origSrc), origSrcPort(origSrcPort), origDst(origDst),
      origDstPort(origDstPort), lastActiveTs(std::chrono::steady_clock::now()) {
    // 四元组 key：源IP:源端口 -> 目的IP:目的端口
    this->sessionKey.assign((const char *)&origSrc, sizeof(uint32_t));
    this->sessionKey.append((const char *)&origSrcPort, sizeof(origSrcPort));
    this->sessionKey.append((const char *)&origDst, sizeof(uint32_t));
    this->sessionKey.append((const char *)&origDstPort, sizeof(origDstPort));
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

    this->fd = (int)::socket(AF_INET, SOCK_DGRAM, 0);
    if (this->fd < 0) {
        spdlog::warn("session udp socket failed: {}", netErrStr(netLastError()));
        return -1;
    }
    netSetNonBlocking(this->fd);

    // connect 固定对端：内核自动填源地址 = 网关 LAN IP（等价 MASQUERADE），
    // 且后续 send/recv 只与该对端往来，形成 Fullcone NAT 的伪会话。
    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(this->origDstPort);
    dst.sin_addr.s_addr = uint32_t(this->origDst);
    if (::connect(this->fd, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        spdlog::warn("session udp connect {}:{} failed: {}", this->origDst.toString(), this->origDstPort,
                     netErrStr(netLastError()));
        netClose(this->fd);
        this->fd = -1;
        return -1;
    }

    spdlog::debug("session udp: {}:{} -> {}:{}", this->origSrc.toString(), this->origSrcPort, this->origDst.toString(),
                  this->origDstPort);

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
    // 本流后续数据报：addr/port=源端(dev1)，数据负载转发到落地 fd。
    if (p == nullptr) {
        return;
    }
    std::string data;
    data.resize(p->tot_len);
    pbuf_copy_partial(p, data.data(), p->tot_len, 0);
    pbuf_free(p);
    sendToLanding(std::move(data));
}

void SessionUdp::sendToLanding(std::string data) {
    // 仅 NetStack 线程调用：刷新活跃时间，把数据报投递到 reactor 线程发送。
    this->lastActiveTs = std::chrono::steady_clock::now();
    auto holder = shared_from_this();
    this->stack->getReactor().post([holder, data = std::move(data)]() mutable {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
        if (holder->fd < 0 || holder->closing.load()) {
            return;
        }
        long n = netSend(holder->fd, data.data(), data.size());
        if (n < 0 && !netWouldBlock(netLastError())) {
            spdlog::debug("session udp send failed: {}", netErrStr(netLastError()));
        }
#endif
    });
}

void SessionUdp::replyToLwip(std::string data) {
    // 仅 NetStack 线程调用：把落地回包以 (origDst:origDstPort -> origSrc:origSrcPort)
    // 重新注入 lwIP，由 netif->output 产生内层 IP 包、再经 NetStack::output 封 IPIP 回源端。
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

    // 源地址固定为落地目的(origDst)，源端口为 origDstPort；目的为源端(origSrc:origSrcPort)。
    ip_addr_t src;
    ip_addr_t dst;
    ip_addr_set_ip4_u32(&src, uint32_t(this->origDst));
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
    if (ev & ReactorEvent::ERROR) {
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
        long n = netRecv(this->fd, buf, sizeof(buf));
        if (n > 0) {
            std::string data(buf, n);
            auto holder = shared_from_this();
            this->stack->postToStack([holder, data = std::move(data)]() mutable { holder->replyToLwip(std::move(data)); });
            continue;
        }
        if (n == 0) {
            return;
        }
        if (netWouldBlock(netLastError())) {
            return;
        }
        // 落地 socket 错误（如 ICMP port unreachable 经 connect 返回 ECONNREFUSED）：关闭会话。
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
