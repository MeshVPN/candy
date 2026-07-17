// SPDX-License-Identifier: MIT
#ifndef CANDY_NETSTACK_UDPMUX_H
#define CANDY_NETSTACK_UDPMUX_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace candy {

class NetStack;

// UdpMux：UDP 单端口收敛的全局多路复用器（见方案 §11）。
//
// 与「每源全锥形」的区别：每源模式下每个内部源(origSrc,origSrcPort)各持一个 unconnected 落地
// fd、各自 recvfrom/sendto；UdpMux 模式下**所有内部源共用一个** unconnected fd、一个出口端口，
// 回程靠下面四张表做 demux 还原到各自内部源。收敛的是「落地 fd」，不收敛「npcb」——lwIP 仍
// 一源一 npcb（保留在 NetStack::udpSessions 作 innerNpcb 定位表），回程注入仍逐源进行。
//
// 性质（前提：双端 NAT3 + 无 TURN）：单端口后过滤维度为「地址且端口依赖过滤(APDF)＝端口受限锥」，
// 恰与物理出口 NAT3 对齐，对端为 NAT3(cone) 时回包端点 R==D 必命中、无损。
//
// 线程契约（§11.3a，必须遵守）：四张表**全部只在 NetStack 线程读写、无锁**。
//   - 发送侧判定在 NetStack 线程（SessionUdp::onPcbRecv → sendFromSource），通过后才 post 到
//     Reactor 线程做 netSendTo；
//   - 接收侧 Reactor 线程只 recvfrom 收字节+对端地址，随即 postToStack 到 NetStack 线程做 demux。
//   Reactor 线程绝不触碰四表。
class UdpMux : public std::enable_shared_from_this<UdpMux> {
public:
    explicit UdpMux(NetStack *stack);
    ~UdpMux();

    // NetStack 线程：建立全局落地 fd（unconnected + bindAny + 调大 SO_RCVBUF），注册单读者到
    // Reactor。成功返回 0，失败返回 -1。
    int start();

    // NetStack 线程：栈拆除（重连）时关闭全局 fd、清空四表。
    void shutdown();

    // NetStack 线程（SessionUdp::onPcbRecv 内）：某内部源 innerSrc 发往远端 D 的一份报文。
    // 内部做 STUN 识别/txid 登记/数据面 FCFS 锁，判定通过后 post 到 Reactor 用全局 fd 发送。
    void sendFromSource(const std::string &innerSrc, uint32_t dstIpBe, uint16_t dstPortHost, std::string data);

    // NetStack 线程：某内部源整体消亡（npcb 回收）时，经反向索引级联清掉该源名下全部
    // endpointOwner 条目，防端点泄漏 + 新源被 FCFS 误挡（§11.3 兜底级联）。
    void onSourceGone(const std::string &innerSrc);

    // NetStack 线程：按端点各自 lastActive 做 LRU 老化 endpointOwner；清理超时 stunTxn。
    // loop 节流调用。
    void reap();

private:
    // ---- Reactor 线程 ----
    void onFdEvent(uint32_t events);
    void onFdReadable();

    // ---- NetStack 线程 ----
    // 收到远端 R=(rIpBe,rPortHost) 的一份回包，按 STUN txid / endpointOwner 反查还原内部源并注入。
    void demux(uint32_t rIpBe, uint16_t rPortHost, std::string data);
    // 丢弃告警限频：保留端点和分类信息，同时避免异常流量刷爆日志。
    void warnDrop(const char *kind, uint32_t peerIpBe, uint16_t peerPortHost);

    // 远端端点 key：(R_ip 网络序) 高 32 位 | (R_port 主机序) 低 16 位。
    static uint64_t endpointKey(uint32_t ipBe, uint16_t portHost) {
        return (uint64_t(ipBe) << 16) | uint64_t(portHost);
    }

private:
    NetStack *stack;
    int fd;
    std::atomic<bool> closing;
    std::shared_ptr<UdpMux> self; // 自持，保证跨线程投递期间存活（对齐 SessionUdp）

    // ---- 四张表（全部仅 NetStack 线程访问，无锁）----

    // 反向 demux 主表：远端数据端点 -> {首占内部源 owner, 该端点自身活跃时间}。FCFS 首占锁定。
    struct OwnerEntry {
        std::string owner; // InnerSrc（= SessionUdp::key，源二元组字节串）
        std::chrono::steady_clock::time_point lastActive;
    };
    std::unordered_map<uint64_t, OwnerEntry> endpointOwner;

    // STUN 事务表：txid(12B) -> {内部源, 登记时间}。Request 登记 / Response 消费 / 超时清理。
    struct TxnEntry {
        std::string innerSrc;
        std::chrono::steady_clock::time_point createdAt;
    };
    std::unordered_map<std::string, TxnEntry> stunTxn;

    // 反向索引：内部源 -> 它占用的全部远端端点集合（供源整体消亡时兜底级联清 endpointOwner）。
    std::unordered_map<std::string, std::unordered_set<uint64_t>> ownerEndpoints;

    // 注：innerNpcb（内部源 -> npcb）复用 NetStack::udpSessions，回程注入经
    // NetStack::injectUdpReply 定位，故此处不再单独维护。

    // ---- 丢弃计数（可观测性，§11.7 点5）----
    uint64_t dropCollision;    // 数据面 FCFS 碰撞（≥2 源发往同一数据端点）
    uint64_t dropTxidMiss;     // STUN Response txid 未命中
    uint64_t dropEndpointMiss; // 数据 / 入站 Request 的 endpoint 未命中
    uint64_t lastLoggedDrops;  // 上次日志时的丢弃总数（仅在增长时打印）
    std::chrono::steady_clock::time_point lastDropWarn;
};

} // namespace candy

#endif
