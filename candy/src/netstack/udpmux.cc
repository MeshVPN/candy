// SPDX-License-Identifier: MIT
#include "netstack/udpmux.h"
#include "netstack/netstack.h"
#include "netstack/sockcompat.h"
#include "netstack/stun.h"
#include "utils/log.h"
#include <Poco/Format.h>
#include <cstring>

namespace candy {

// endpointOwner 按端点 LRU 老化阈值：必须 ≤ 物理出口 NAT3 的 UDP 映射老化（运营商常 30–60s）。
// 无 TURN 兜底，故不能比物理层活得久（否则 lwIP 以为映射还在、物理层已老化，回包被物理 NAT 丢）。
static constexpr std::chrono::seconds kEndpointIdle{30};
// STUN 事务表超时：覆盖 RFC5389 重传窗口（Rc*RTO≈39.5s），略给余量，防 Response 丢失致表膨胀。
static constexpr std::chrono::seconds kStunTxnIdle{40};
// 单出口 fd 接收缓冲：单 socket 收全部对端回包，突发海量流需大缓冲防溢出丢包。
static constexpr int kRecvBufBytes = 4 * 1024 * 1024;
// 未命中可能由异常对端高频触发；保留 WARN 可观测性，同时限频保护日志系统。
static constexpr std::chrono::seconds kDropWarnInterval{1};

UdpMux::UdpMux(NetStack *stack)
    : stack(stack), fd(-1), closing(false), dropCollision(0), dropTxidMiss(0), dropEndpointMiss(0), lastLoggedDrops(0) {}

UdpMux::~UdpMux() {
    if (this->fd >= 0) {
        netClose(this->fd);
        this->fd = -1;
    }
}

int UdpMux::start() {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
    // 全局唯一落地 fd：unconnected + bindAny（固定出口端口，全生命周期不变），所有内部源复用。
    this->fd = this->stack->getOutbound().dialUdp();
    if (this->fd < 0) {
        candy::logger().error("udpmux dialUdp failed");
        return -1;
    }
    // 收敛后单 fd 承载全部回包，显式调大接收缓冲防溢出（原多 socket 天然分摊，无此需求）。
    if (netSetRecvBuf(this->fd, kRecvBufBytes) != 0) {
        candy::logger().warning(Poco::format("udpmux set SO_RCVBUF failed: %s", netErrStr(netLastError())));
    }

    candy::logger().information("udpmux started: single-port UDP convergence enabled");

    // fd 已成功创建后才建立自持引用；失败路径不产生 shared_ptr 环，确保调用方 reset 后可析构。
    this->self = shared_from_this();
    auto holder = this->self;
    int f = this->fd;
    this->stack->getReactor().add(f, ReactorEvent::READ, [holder](ReactorEvent ev) { holder->onFdEvent((uint32_t)ev); });
    return 0;
#else
    return -1;
#endif
}

void UdpMux::shutdown() {
    // NetStack 线程调用：清空四表（本线程独占，无锁），fd 交由 reactor 关闭。
    this->endpointOwner.clear();
    this->stunTxn.clear();
    this->ownerEndpoints.clear();

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

// ===================== NetStack 线程：发送侧 =====================

void UdpMux::sendFromSource(const std::string &innerSrc, uint32_t dstIpBe, uint16_t dstPortHost, std::string data) {
    auto now = std::chrono::steady_clock::now();
    uint64_t k = endpointKey(dstIpBe, dstPortHost);

    // step 1：STUN 一律豁免数据面 FCFS（不看具体 method/class）。见 §11.4 发送 step 1。
    StunInfo info;
    if (stunParse(data.data(), data.size(), &info)) {
        if (info.klass == StunClass::Request) {
            // 带事务的 Request：登记 txid → 内部源，供回程 Response 靠 txid 分流。
            // keepalive(Indication) 等无需回程配对的消息：豁免但不记 txid。
            std::string txid((const char *)info.txid, sizeof(info.txid));
            this->stunTxn[txid] = TxnEntry{innerSrc, now};
        }
        // 对 endpointOwner：仅「存在且同源则保活」，不含则不创建（避免把共享 STUN 端点锁给某源），
        // 异源则跳过、不丢（否则多源共享同一公共 STUN 会被误判碰撞丢弃）。
        auto it = this->endpointOwner.find(k);
        if (it != this->endpointOwner.end() && it->second.owner == innerSrc) {
            it->second.lastActive = now;
        }
        // step 3：直接发送（必须跳过 step 2 的 FCFS）。
    } else {
        // step 2：数据包端点归属（数据面 FCFS 锁定）。
        auto it = this->endpointOwner.find(k);
        if (it == this->endpointOwner.end()) {
            // 首占，锁定。
            this->endpointOwner[k] = OwnerEntry{innerSrc, now};
            this->ownerEndpoints[innerSrc].insert(k);
        } else if (it->second.owner != innerSrc) {
            // 碰撞：D 已被别的源占用（禁区 A）——只丢后者，绝不影响首占者。
            ++this->dropCollision;
            candy::logger().warning(Poco::format(
                "udpmux drop(collision): dst=%08x:%hu owned by other source, dropping later source's packet", dstIpBe,
                dstPortHost));
            return;
        } else {
            // 同源续发：保活（对端 NAT3，keepalive 维持物理映射，命脉）。
            it->second.lastActive = now;
        }
    }

    // step 3：post 到 Reactor 线程用全局 fd 发送（表操作已在 NetStack 线程完成）。
    auto holder = shared_from_this();
    this->stack->getReactor().post([holder, dstIpBe, dstPortHost, data = std::move(data)]() mutable {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
        if (holder->closing.load() || holder->fd < 0) {
            return;
        }
        long n = netSendTo(holder->fd, data.data(), data.size(), dstIpBe, dstPortHost);
        if (n < 0 && !netWouldBlock(netLastError())) {
            candy::logger().debug(Poco::format("udpmux sendto failed: %s", netErrStr(netLastError())));
        }
#endif
    });
}

void UdpMux::onSourceGone(const std::string &innerSrc) {
    // 源整体消亡（npcb 回收）：经反向索引级联清掉该源名下全部 endpointOwner 条目（§11.3 兜底）。
    auto it = this->ownerEndpoints.find(innerSrc);
    if (it == this->ownerEndpoints.end()) {
        return;
    }
    for (uint64_t k : it->second) {
        auto eit = this->endpointOwner.find(k);
        // 仅删仍属本源的条目（老化后可能已被新首占者重锁，不能误删）。
        if (eit != this->endpointOwner.end() && eit->second.owner == innerSrc) {
            this->endpointOwner.erase(eit);
        }
    }
    this->ownerEndpoints.erase(it);
}

void UdpMux::warnDrop(const char *kind, uint32_t peerIpBe, uint16_t peerPortHost) {
    auto now = std::chrono::steady_clock::now();
    if (now - this->lastDropWarn < kDropWarnInterval) {
        return;
    }
    this->lastDropWarn = now;
    candy::logger().warning(Poco::format(
        "udpmux drop(%s): peer=%08x:%hu totals(collision=%s txidMiss=%s endpointMiss=%s)", kind, peerIpBe,
        peerPortHost, std::to_string(this->dropCollision), std::to_string(this->dropTxidMiss),
        std::to_string(this->dropEndpointMiss)));
}

void UdpMux::reap() {
    auto now = std::chrono::steady_clock::now();

    // endpointOwner：按端点各自 lastActive 做 LRU 老化（对齐真实 NAT 逐映射独立超时）。
    for (auto it = this->endpointOwner.begin(); it != this->endpointOwner.end();) {
        if (now - it->second.lastActive > kEndpointIdle) {
            // 同步从反向索引摘除该端点。
            auto oit = this->ownerEndpoints.find(it->second.owner);
            if (oit != this->ownerEndpoints.end()) {
                oit->second.erase(it->first);
                if (oit->second.empty()) {
                    this->ownerEndpoints.erase(oit);
                }
            }
            it = this->endpointOwner.erase(it);
        } else {
            ++it;
        }
    }

    // stunTxn：超时清理（防 Response 丢失导致表膨胀）。
    for (auto it = this->stunTxn.begin(); it != this->stunTxn.end();) {
        if (now - it->second.createdAt > kStunTxnIdle) {
            it = this->stunTxn.erase(it);
        } else {
            ++it;
        }
    }

    // 丢弃计数仅在增长时打印（可观测：判断是否频繁踩禁区）。
    uint64_t total = this->dropCollision + this->dropTxidMiss + this->dropEndpointMiss;
    if (total != this->lastLoggedDrops) {
        candy::logger().information(Poco::format(
            "udpmux stats: endpoints=%s txns=%s drops(collision=%s txidMiss=%s endpointMiss=%s)",
            std::to_string(this->endpointOwner.size()), std::to_string(this->stunTxn.size()),
            std::to_string(this->dropCollision), std::to_string(this->dropTxidMiss),
            std::to_string(this->dropEndpointMiss)));
        this->lastLoggedDrops = total;
    }
}

// ===================== Reactor 线程：单读者 =====================

void UdpMux::onFdEvent(uint32_t events) {
    ReactorEvent ev = (ReactorEvent)events;
    if (ev & ReactorEvent::FAILURE) {
        // 全局 fd 是收敛命脉，不能因偶发错误关闭；记录后继续（unconnected 下罕见）。
        candy::logger().warning("udpmux fd failure event");
        return;
    }
    if (ev & ReactorEvent::READ) {
        onFdReadable();
    }
}

void UdpMux::onFdReadable() {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
    char buf[65536];
    while (true) {
        // 单 socket 收任意对端回包，取回真实对端地址/端口，连同数据投递到 NetStack 线程做 demux。
        uint32_t peerIpBe;
        uint16_t peerPortHost;
        long n = netRecvFrom(this->fd, buf, sizeof(buf), &peerIpBe, &peerPortHost);
        if (n > 0) {
            std::string data(buf, n);
            auto holder = shared_from_this();
            this->stack->postToStack([holder, peerIpBe, peerPortHost, data = std::move(data)]() mutable {
                holder->demux(peerIpBe, peerPortHost, std::move(data));
            });
            continue;
        }
        if (n == 0) {
            return;
        }
        if (netWouldBlock(netLastError())) {
            return;
        }
        // 罕见错误：记录但保留 fd（不像 per-session 那样关闭）。
        candy::logger().debug(Poco::format("udpmux recvfrom failed: %s", netErrStr(netLastError())));
        return;
    }
#endif
}

// ===================== NetStack 线程：接收侧 demux =====================

void UdpMux::demux(uint32_t rIpBe, uint16_t rPortHost, std::string data) {
    auto now = std::chrono::steady_clock::now();
    uint64_t k = endpointKey(rIpBe, rPortHost);

    // step 1：STUN 按类型分流。
    StunInfo info;
    if (stunParse(data.data(), data.size(), &info)) {
        if (info.klass == StunClass::SuccessResp || info.klass == StunClass::ErrorResp) {
            // 1a：Response/Error 走 txid 反查。
            std::string txid((const char *)info.txid, sizeof(info.txid));
            auto it = this->stunTxn.find(txid);
            if (it != this->stunTxn.end()) {
                std::string innerSrc = it->second.innerSrc;
                this->stunTxn.erase(it);
                this->stack->injectUdpReply(innerSrc, rIpBe, rPortHost, std::move(data));
                return;
            }
            ++this->dropTxidMiss; // txid 对不上：非本机发起或已超时。
            this->warnDrop("stun-txid-miss", rIpBe, rPortHost);
            return;
        }
        // 1b：入站 Request/Indication（对端 ICE 连通性检查）——txid 由对端生成，不在 stunTxn，
        //     必须走 endpointOwner（前提：我方已先向 R 发过包并 FCFS 锁定）。
        auto it = this->endpointOwner.find(k);
        if (it != this->endpointOwner.end()) {
            it->second.lastActive = now;
            this->stack->injectUdpReply(it->second.owner, rIpBe, rPortHost, std::move(data));
            return;
        }
        ++this->dropEndpointMiss; // 我方未先发起，无归属。
        this->warnDrop("stun-endpoint-miss", rIpBe, rPortHost);
        return;
    }

    // step 2：数据包走 endpointOwner。
    auto it = this->endpointOwner.find(k);
    if (it != this->endpointOwner.end()) {
        it->second.lastActive = now; // 接收侧保活：对端单向来包也算活跃，防误回收。
        this->stack->injectUdpReply(it->second.owner, rIpBe, rPortHost, std::move(data));
        return;
    }
    // 未命中：对端冷启动主动连 / 已老化 / 禁区碰撞受害包（对端为 NAT3 时 R==D 不会误丢）。
    ++this->dropEndpointMiss;
    this->warnDrop("data-endpoint-miss", rIpBe, rPortHost);
}

} // namespace candy
