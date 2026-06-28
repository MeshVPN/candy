// SPDX-License-Identifier: MIT
#ifndef CANDY_NETSTACK_SESSION_UDP_H
#define CANDY_NETSTACK_SESSION_UDP_H

#include "core/net.h"
#include "netstack/session.h"
#include "netstack/tcp_handshake.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "lwip/udp.h"

namespace candy {

// SessionUdp：一条 UDP "伪会话"——把 lwIP 终结的某个 (源,目的) 四元组数据流，
// 映射到一个 connect 到目的的内核 UDP socket 落地（Fullcone NAT 思路）。
//
// 线程契约（与 SessionTcp 一致）：
//   - 所有 udp_*(pcb) 调用只在 NetStack 线程执行；
//   - 所有 fd 的 recv/send 只在 Reactor 线程执行；
//   - 两侧通过 NetStack::postToStack / Reactor::post 投递任务跨线程交互。
//
// 数据流：
//   正向 lwIP->fd：udp_recv 回调(NetStack线程) 收数据报 -> 投递 reactor send(fd)
//   反向 fd->lwIP：fd 可读(reactor线程) recv -> 投递 NetStack udp_sendto_if_src 回送源端
class SessionUdp : public Session, public std::enable_shared_from_this<SessionUdp> {
public:
    // pcb：lwIP 为该四元组克隆出的已 connect 的 udp_pcb（NetStack 线程持有）。
    // origSrc/origSrcPort：源端(dev1)；origDst/origDstPort：落地目的(dev2 服务)。
    SessionUdp(NetStack *stack, struct udp_pcb *pcb, IP4 origSrc, uint16_t origSrcPort, IP4 origDst,
               uint16_t origDstPort);
    ~SessionUdp() override;

    int start() override;

    // 仅供 NetStack 线程在栈拆除（重连）时调用，强制关闭 pcb 与 fd。
    void shutdownFromStack();

    // NetStack 线程：把一份来自 lwIP 的数据报投递到落地 fd 发送。
    void sendToLanding(std::string data);

    // 该会话的四元组 key（NetStack 线程的会话表用）。
    const std::string &key() const { return this->sessionKey; }

    // 最近活跃时间（用于空闲超时回收，仅 NetStack 线程访问）。
    std::chrono::steady_clock::time_point lastActive() const { return this->lastActiveTs; }

    // lwIP 每流 recv 跳板：克隆出的 npcb 后续同流数据报都走这里（仅 NetStack 线程）。
    static void recvTrampoline(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);

private:
    // ---- Reactor 线程 ----
    void onFdEvent(uint32_t events);
    void onFdReadable();
    void closeFromReactor();

    // socks5 UDP ASSOCIATE 分支（均在 Reactor 线程驱动）：
    //   控制连接(ctrlFd)：connect 完成 -> 跑 UDP ASSOCIATE 握手 -> 拿到中继端点 ->
    //   建中继 UDP socket(fd) connect 到中继端点；之后 fd 上收发都加/去 socks5 UDP 头。
    //   ctrlFd 保活，其关闭即关联失效，会话关闭。
    void onCtrlEvent(uint32_t events);
    void onCtrlConnected();
    void onCtrlReadable();
    void onCtrlWritable();
    void flushCtrlOutbound();
    void finishAssociate();

    // ---- NetStack 线程 ----
    // 本流后续数据报到达：addr/port=源端(dev1)，提取数据投递到落地 fd 发送。
    void onPcbRecv(struct pbuf *p, const ip_addr_t *addr, u16_t port);
    // 落地 fd 收到回包后，用 pcb 把数据报回送源端。
    void replyToLwip(std::string data);
    void closeFromStack();

private:
    struct udp_pcb *pcb;
    int fd;

    // ---- socks5 UDP ASSOCIATE 专用（direct 路径全程为默认值，不影响行为）----
    bool useSocks5 = false;          // 命中 socks5 出站（需 UDP ASSOCIATE）
    int ctrlFd = -1;                 // socks5 TCP 控制连接 fd（保活）
    bool ctrlConnected = false;      // 控制连接 connect 是否完成
    std::unique_ptr<TcpHandshake> associate; // UDP ASSOCIATE 握手器（完成后置空）
    std::string ctrlOutbox;          // 控制连接待发字节残留（非阻塞部分写）
    bool relayReady = false;         // 中继 fd 是否已就绪（握手完成、connect 中继后置真）
    IP4 relayHost;                   // socks5 UDP 中继端点
    uint16_t relayPort = 0;
    IP4 proxyServerHost;             // socks5 server IP（中继地址为 0.0.0.0 时回退用）
    std::string pendingDatagrams;    // 握手期间 lwIP 侧到达的数据报暂存（带长度前缀串联）

    IP4 origSrc;
    uint16_t origSrcPort;
    IP4 origDst;
    uint16_t origDstPort;

    std::string sessionKey;
    std::chrono::steady_clock::time_point lastActiveTs;

    std::shared_ptr<SessionUdp> self;
};

} // namespace candy

#endif
