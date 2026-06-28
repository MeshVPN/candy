// SPDX-License-Identifier: MIT
#include "netstack/session_udp.h"
#include "netstack/netstack.h"
#include "netstack/sockcompat.h"
#include "netstack/socks5_client.h"
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
    if (this->ctrlFd >= 0) {
        netClose(this->ctrlFd);
        this->ctrlFd = -1;
    }
}

int SessionUdp::start() {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
    this->self = shared_from_this();

    // 按流分流：Router.match 选出 outbound（direct/socks5）。
    // 无规则时恒为 DirectOutbound，与阶段一/二行为完全一致（零回归）。
    Router::FlowKey flow{this->origSrc, this->origDst, IPPROTO_UDP, this->origDstPort};
    Outbound &outbound = this->stack->getOutbound(flow);

    if (outbound.udpNeedsAssociation()) {
        // ---- socks5 UDP ASSOCIATE 分支 ----
        // 先建到 socks5 server 的 TCP 控制连接（保活），connect 完成后在其上跑 UDP
        // ASSOCIATE 握手；拿到中继端点再建中继 UDP socket。期间到达的数据报暂存。
        this->useSocks5 = true;
        Endpoint ps = outbound.proxyServerEndpoint();
        this->proxyServerHost = ps.host;

        this->ctrlFd = outbound.dialUdpControl();
        if (this->ctrlFd < 0) {
            return -1;
        }
        this->associate = outbound.makeUdpAssociate();
        if (!this->associate) {
            return -1;
        }

        spdlog::debug("session udp(socks5): {}:{} -> {}:{} via {}:{}", this->origSrc.toString(), this->origSrcPort,
                      this->origDst.toString(), this->origDstPort, ps.host.toString(), ps.port);

        auto holder = shared_from_this();
        int ctrlFd = this->ctrlFd;
        // 控制连接非阻塞 connect：先等可写（connect 完成）。
        this->stack->getReactor().add(ctrlFd, ReactorEvent::WRITE,
                                      [holder](ReactorEvent ev) { holder->onCtrlEvent((uint32_t)ev); });
        // 注册每流 recv：握手未完成时数据报会被暂存，完成后冲刷到中继 fd。
        udp_recv(this->pcb, recvTrampoline, this);
        return 0;
    }

    // ---- direct 分支（阶段一/二原逻辑，零回归）----
    // 落地拨号交给选中的 Outbound（DirectOutbound: connect 固定对端，
    // 内核自动填源地址 = 网关 LAN IP，等价 MASQUERADE，形成 Fullcone NAT 伪会话）。
    this->fd = outbound.dialUdp(Endpoint{this->origDst, this->origDstPort});
    if (this->fd < 0) {
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
        if (holder->closing.load()) {
            return;
        }
        if (holder->useSocks5) {
            // socks5 中继：每个业务数据报封装 socks5 UDP 头（目的=origDst:origDstPort）。
            std::string pkt = socks5UdpEncap(holder->origDst, holder->origDstPort,
                                             reinterpret_cast<const uint8_t *>(data.data()), data.size());
            if (!holder->relayReady) {
                // 握手尚未完成：暂存（带 2 字节长度前缀串联），完成后由 finishAssociate 冲刷。
                uint16_t n = (uint16_t)pkt.size();
                holder->pendingDatagrams.append(reinterpret_cast<const char *>(&n), sizeof(n));
                holder->pendingDatagrams.append(pkt);
                return;
            }
            if (holder->fd < 0) {
                return;
            }
            long n = netSend(holder->fd, pkt.data(), pkt.size());
            if (n < 0 && !netWouldBlock(netLastError())) {
                spdlog::debug("session udp(socks5) send failed: {}", netErrStr(netLastError()));
            }
            return;
        }
        if (holder->fd < 0) {
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
            if (holder->ctrlFd >= 0) {
                holder->stack->getReactor().del(holder->ctrlFd);
                netClose(holder->ctrlFd);
                holder->ctrlFd = -1;
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
            if (this->useSocks5) {
                // socks5 中继回包：去掉 socks5 UDP 头，仅把业务数据灌回 lwIP。
                IP4 srcHost;
                uint16_t srcPort = 0;
                const uint8_t *payload = nullptr;
                size_t payloadLen = 0;
                if (!socks5UdpDecap(reinterpret_cast<const uint8_t *>(buf), (size_t)n, srcHost, srcPort, &payload,
                                    payloadLen)) {
                    spdlog::debug("session udp(socks5) drop malformed relay datagram len={}", n);
                    continue;
                }
                std::string data(reinterpret_cast<const char *>(payload), payloadLen);
                auto holder = shared_from_this();
                this->stack->postToStack([holder, data = std::move(data)]() mutable { holder->replyToLwip(std::move(data)); });
                continue;
            }
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
    if (this->ctrlFd >= 0) {
        this->stack->getReactor().del(this->ctrlFd);
        netClose(this->ctrlFd);
        this->ctrlFd = -1;
    }
#endif
    if (this->pcb != nullptr) {
        auto holder = shared_from_this();
        this->stack->postToStack([holder] { holder->closeFromStack(); });
    }
    auto keep = this->self;
    this->self.reset();
}

// ===================== socks5 UDP ASSOCIATE 控制连接（Reactor 线程） =====================

void SessionUdp::onCtrlEvent(uint32_t events) {
    ReactorEvent ev = (ReactorEvent)events;
    if (!this->ctrlConnected) {
        if (ev & ReactorEvent::ERROR) {
            closeFromReactor();
            return;
        }
        if (ev & ReactorEvent::WRITE) {
            onCtrlConnected();
        }
        return;
    }
    if (ev & ReactorEvent::ERROR) {
        // 控制连接断开即关联失效，关闭整条会话。
        closeFromReactor();
        return;
    }
    // 握手阶段：分流可读/可写。握手完成后 associate 置空，控制连接仅保活（只关注 ERROR）。
    if (this->associate) {
        if (ev & ReactorEvent::READ) {
            onCtrlReadable();
        }
        if (this->associate && (ev & ReactorEvent::WRITE)) {
            onCtrlWritable();
        }
        return;
    }
    // 关联已建立：控制连接上若有可读（通常不应有业务数据），读掉即可；对端关闭则关会话。
    if (ev & ReactorEvent::READ) {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
        char buf[1024];
        long n = netRecv(this->ctrlFd, buf, sizeof(buf));
        if (n == 0) {
            closeFromReactor();
            return;
        }
#endif
    }
}

void SessionUdp::onCtrlConnected() {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
    int err = 0;
#if defined(_WIN32) || defined(_WIN64)
    int len = sizeof(err);
    int ret = ::getsockopt((SOCKET)this->ctrlFd, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
#else
    socklen_t len = sizeof(err);
    int ret = ::getsockopt(this->ctrlFd, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
    if (ret != 0 || err != 0) {
        spdlog::warn("session udp(socks5) ctrl connect error: {}", netErrStr(err ? err : netLastError()));
        closeFromReactor();
        return;
    }
    this->ctrlConnected = true;
    spdlog::debug("session udp(socks5) ctrl connected, begin UDP ASSOCIATE handshake");
    this->stack->getReactor().mod(this->ctrlFd, ReactorEvent::READ | ReactorEvent::WRITE);
    flushCtrlOutbound();
#endif
}

void SessionUdp::flushCtrlOutbound() {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
    if (!this->associate || this->ctrlFd < 0) {
        return;
    }
    if (this->associate->hasOutbound()) {
        std::vector<uint8_t> out = this->associate->takeOutbound();
        this->ctrlOutbox.append(reinterpret_cast<const char *>(out.data()), out.size());
    }
    while (!this->ctrlOutbox.empty()) {
        long n = netSend(this->ctrlFd, this->ctrlOutbox.data(), this->ctrlOutbox.size());
        if (n > 0) {
            this->ctrlOutbox.erase(0, n);
            continue;
        }
        if (n < 0 && netWouldBlock(netLastError())) {
            this->stack->getReactor().mod(this->ctrlFd, ReactorEvent::READ | ReactorEvent::WRITE);
            return;
        }
        spdlog::warn("session udp(socks5) ctrl send failed: {}", netErrStr(netLastError()));
        closeFromReactor();
        return;
    }
    this->stack->getReactor().mod(this->ctrlFd, ReactorEvent::READ);
#endif
}

void SessionUdp::onCtrlWritable() {
    flushCtrlOutbound();
}

void SessionUdp::onCtrlReadable() {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
    if (!this->associate) {
        return;
    }
    char buf[4096];
    while (true) {
        long n = netRecv(this->ctrlFd, buf, sizeof(buf));
        if (n > 0) {
            if (!this->associate->feed(reinterpret_cast<const uint8_t *>(buf), (size_t)n)) {
                spdlog::warn("session udp(socks5) associate failed: {}", this->associate->error());
                closeFromReactor();
                return;
            }
            if (this->associate->done()) {
                finishAssociate();
                return;
            }
            if (this->associate->hasOutbound()) {
                flushCtrlOutbound();
            }
            continue;
        }
        if (n == 0) {
            spdlog::warn("session udp(socks5) server closed during associate");
            closeFromReactor();
            return;
        }
        if (netWouldBlock(netLastError())) {
            return;
        }
        spdlog::warn("session udp(socks5) ctrl recv failed: {}", netErrStr(netLastError()));
        closeFromReactor();
        return;
    }
#endif
}

void SessionUdp::finishAssociate() {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
    // 关联成功：取中继端点。若服务端返回 0.0.0.0（RFC 1928 §7 允许），回退到 server IP。
    this->relayHost = this->associate->udpRelayHost();
    this->relayPort = this->associate->udpRelayPort();
    if (uint32_t(this->relayHost) == 0) {
        this->relayHost = this->proxyServerHost;
    }
    this->associate.reset();
    // 控制连接转为保活：仅关注 ERROR（其断开即关联失效）。
    this->stack->getReactor().mod(this->ctrlFd, ReactorEvent::NONE);

    spdlog::debug("session udp(socks5) associate done, relay {}:{}", this->relayHost.toString(), this->relayPort);

    // 建中继 UDP socket，connect 到中继端点（之后 send/recv 都是 socks5 UDP 封装报文）。
    int fd = (int)::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        spdlog::warn("session udp(socks5) relay socket failed: {}", netErrStr(netLastError()));
        closeFromReactor();
        return;
    }
    netSetNonBlocking(fd);
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(this->relayPort);
    addr.sin_addr.s_addr = uint32_t(this->relayHost);
    if (::connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        spdlog::warn("session udp(socks5) relay connect {}:{} failed: {}", this->relayHost.toString(), this->relayPort,
                     netErrStr(netLastError()));
        netClose(fd);
        closeFromReactor();
        return;
    }
    this->fd = fd;
    this->relayReady = true;
    auto holder = shared_from_this();
    this->stack->getReactor().add(fd, ReactorEvent::READ, [holder](ReactorEvent ev) { holder->onFdEvent((uint32_t)ev); });

    // 冲刷握手期间暂存的数据报（带 2 字节长度前缀串联）。
    size_t off = 0;
    const std::string &buf = this->pendingDatagrams;
    while (off + sizeof(uint16_t) <= buf.size()) {
        uint16_t len = 0;
        std::memcpy(&len, buf.data() + off, sizeof(len));
        off += sizeof(len);
        if (off + len > buf.size()) {
            break;
        }
        long n = netSend(this->fd, buf.data() + off, len);
        if (n < 0 && !netWouldBlock(netLastError())) {
            spdlog::debug("session udp(socks5) flush pending send failed: {}", netErrStr(netLastError()));
        }
        off += len;
    }
    this->pendingDatagrams.clear();
#endif
}

} // namespace candy
