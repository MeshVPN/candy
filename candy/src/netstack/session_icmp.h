// SPDX-License-Identifier: MIT
#ifndef CANDY_NETSTACK_SESSION_ICMP_H
#define CANDY_NETSTACK_SESSION_ICMP_H

#include "core/net.h"
#include "netstack/session.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace candy {

// SessionIcmp：一条 ICMP echo "伪会话"——把某个 (源,目的,icmp-id) 的 ping 流，
// 映射到一个 connect 到目的的内核 ICMP socket 落地（等价 iptables MASQUERADE）。
//
// 与 TCP/UDP 不同，ICMP 不走 lwIP：lwIP 的 PRETEND netif 只接受 TCP/UDP，
// 故 ICMP echo 在入栈前(NetStack::handleInput)被直接拦截，由本会话落地+回包。
//
// 线程契约（与 SessionUdp 一致）：
//   - 所有跨线程交互通过 NetStack::postToStack / Reactor::post；
//   - fd 的 send/recv 只在 Reactor 线程；回包构造在 NetStack 线程。
//
// 数据流：
//   正向：handleIcmp(NetStack线程) 提取 echo request -> 投递 reactor send(fd)
//   反向：fd 可读(reactor线程) recv echo reply -> 投递 NetStack 还原 id/校验和
//         -> 手工封 IP 包 -> NetStack::output 封 IPIP 回源端
class SessionIcmp : public Session, public std::enable_shared_from_this<SessionIcmp> {
public:
    // origSrc/origDst：源端(dev1)/落地目的(dev2)；icmpId：源端 ping 进程的 icmp id。
    SessionIcmp(NetStack *stack, IP4 origSrc, IP4 origDst, uint16_t icmpId);
    ~SessionIcmp() override;

    int start() override;

    // 仅供 NetStack 线程在栈拆除/空闲回收时调用，强制关闭 fd。
    void shutdownFromStack();

    // NetStack 线程：把一份 echo request 的 ICMP 报文(含 icmp 头)投递到落地 fd 发送。
    void sendEcho(std::string icmpData);

    // 该会话的 key（NetStack 线程的会话表用）。
    const std::string &key() const { return this->sessionKey; }

    // 最近活跃时间（空闲超时回收，仅 NetStack 线程访问）。
    std::chrono::steady_clock::time_point lastActive() const { return this->lastActiveTs; }

private:
    // ---- Reactor 线程 ----
    void onFdEvent(uint32_t events);
    void onFdReadable();
    void closeFromReactor();

    // ---- NetStack 线程 ----
    // 落地 fd 收到 echo reply 后：还原原始 icmp id、重算校验和、封 IP 头回送源端。
    void replyToPeer(std::string icmpReply);

private:
    int fd;
    bool rawMode;

    IP4 origSrc;
    IP4 origDst;
    uint16_t icmpId;

    std::string sessionKey;
    std::chrono::steady_clock::time_point lastActiveTs;

    std::shared_ptr<SessionIcmp> self;
};

} // namespace candy

#endif
