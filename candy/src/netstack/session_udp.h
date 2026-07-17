// SPDX-License-Identifier: MIT
#ifndef CANDY_NETSTACK_SESSION_UDP_H
#define CANDY_NETSTACK_SESSION_UDP_H

#include "core/net.h"
#include "netstack/session.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

// Windows：在 lwIP 头之前引入 winsock（见 netstack.h 说明），避免 htonl 宏改写导致声明冲突。
#include "netstack/sockcompat.h"

#include "lwip/udp.h"

namespace candy {

// SessionUdp：一条 UDP 源端点会话——把 lwIP 终结的某个源二元组 (源IP,源端口) 的全部
// 出站数据流，映射到一个 unconnected 的内核 UDP socket 落地，实现全锥形（Full Cone）NAT。
//
// 全锥形要点：
//   - 落地 fd 不 connect 固定目的，bind 一个固定出口端口后用 sendto 发往任意多目的、
//     用 recvfrom 收任意对端回包 → endpoint-independent mapping + filtering。
//   - lwIP 的 PRETEND netif 天然「一源一 npcb」：同一源二元组无论发往多少不同目的都命中
//     同一 npcb，每个数据报的目的由 lwIP 覆写进 pcb->local_ip/local_port，故目的是 per-packet
//     从回调现场读取，不属于会话本体。
//   - 回包注入时用「实际对端」作源地址、并临时写 npcb->local_port 作 UDP 源端口，从而把来自
//     陌生对端（P2P 对端 / STUN）的包反向注入回源端，支撑 UDP 打洞。
//
// 线程契约（与 SessionTcp 一致）：
//   - 所有 udp_*(pcb) 调用只在 NetStack 线程执行；
//   - 所有 fd 的 recv/send 只在 Reactor 线程执行；
//   - 两侧通过 NetStack::postToStack / Reactor::post 投递任务跨线程交互。
//
// 数据流：
//   正向 lwIP->fd：udp_recv 回调(NetStack线程) 收数据报 + 读 pcb->local_* 得目的 -> 投递 reactor sendto(fd)
//   反向 fd->lwIP：fd 可读(reactor线程) recvfrom 得真实对端 -> 投递 NetStack udp_sendto_if_src 回送源端
class SessionUdp : public Session, public std::enable_shared_from_this<SessionUdp> {
public:
    // pcb：lwIP 为该源二元组克隆出的 npcb（NetStack 线程持有，其 local_* 随每个入站包被 lwIP
    //      覆写为当前目的）。origSrc/origSrcPort：源端(dev1)。
    // converged：是否单端口收敛模式。false＝每源全锥形（本会话自建 fd、自 recvfrom）；
    //      true＝所有源共用全局 UdpMux 的单 fd（本会话不建 fd，发送经 UdpMux、回程由 UdpMux
    //      demux 后回调本会话 replyToLwip）。
    SessionUdp(NetStack *stack, struct udp_pcb *pcb, IP4 origSrc, uint16_t origSrcPort, bool converged);
    ~SessionUdp() override;

    int start() override;

    // 仅供 NetStack 线程在栈拆除（重连）时调用，强制关闭 pcb 与 fd。
    void shutdownFromStack();

    // NetStack 线程：把一份来自 lwIP 的数据报连同其目的（per-packet，从 pcb->local_* 读得）
    // 投递到落地 fd，用 sendto 发往该目的。
    void sendToLanding(uint32_t dstIpBe, uint16_t dstPortHost, std::string data);

    // 落地 fd 收到某对端(peerIp:peerPort)回包后，用 pcb 以该对端为源把数据报注入回源端。
    // 每源模式：本会话 onFdReadable 内经 postToStack 自调；
    // 收敛模式：UdpMux demux 出本源后经 NetStack::injectUdpReply 调用。均在 NetStack 线程。
    void replyToLwip(uint32_t peerIpBe, uint16_t peerPortHost, std::string data);

    // 该会话的源二元组 key（NetStack 线程的会话表用）。
    const std::string &key() const {
        return this->sessionKey;
    }

    // 最近活跃时间（用于空闲超时回收，仅 NetStack 线程访问）。
    std::chrono::steady_clock::time_point lastActive() const {
        return this->lastActiveTs;
    }

    // lwIP 每流 recv 跳板：克隆出的 npcb 后续同流数据报都走这里（仅 NetStack 线程）。
    static void recvTrampoline(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);

private:
    // ---- Reactor 线程 ----
    void onFdEvent(uint32_t events);
    void onFdReadable();
    void closeFromReactor();

    // ---- NetStack 线程 ----
    // 本流后续数据报到达：addr/port=源端(dev1)，目的从 pcb->local_* 读取，投递到落地 fd 发送。
    void onPcbRecv(struct pbuf *p, const ip_addr_t *addr, u16_t port);
    void closeFromStack();

private:
    struct udp_pcb *pcb;
    int fd;

    // 单端口收敛模式：true＝不自建 fd，发送经全局 UdpMux、回程由 UdpMux 回调 replyToLwip。
    bool converged;

    IP4 origSrc;
    uint16_t origSrcPort;

    std::string sessionKey;
    std::chrono::steady_clock::time_point lastActiveTs;

    std::shared_ptr<SessionUdp> self;
};

} // namespace candy

#endif
