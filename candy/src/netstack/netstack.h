// SPDX-License-Identifier: MIT
#ifndef CANDY_NETSTACK_NETSTACK_H
#define CANDY_NETSTACK_NETSTACK_H

#include "core/net.h"
#include "netstack/outbound.h"
#include "netstack/reactor.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>

#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"

namespace candy {

class Client;
class SessionTcp;
class SessionUdp;

// NetStack：独占 lwIP（NO_SYS=1 单线程 raw API）的模块。
// 持有一个 NetStack 线程驱动 lwIP 收发与定时器，一个 Reactor 线程管理落地 fd。
// 落地端在 userspace 模式下，把 IPIP 内层包喂进 lwIP 终结，TCP 由内核 socket 直连目标。
class NetStack {
public:
    NetStack();
    ~NetStack();

    int run(Client *client);
    int wait();
    void shutdown();

    // 把待终结的 IP 包投递给 NetStack（来自 tun 落地分支，含 IPIP 外层）。
    void input(std::string packet);

    // 供 Session 使用：在 NetStack 线程内执行任务（保证 lwIP API 线程安全）。
    void postToStack(std::function<void()> task);
    Reactor &getReactor();
    // 落地拨号出站：当前仅支持内核 socket 直连（DirectOutbound）。
    Outbound &getOutbound();
    // UDP 回包注入用：拿到本模块的 lwIP netif（仅 NetStack 线程使用）。
    struct netif &getNetif();

    // 回包：lwIP netif->output 产生的 IP 包，按连接上下文重新 IPIP 封装送回源端。
    void output(const std::string &innerPacket);

    // Session 结束时从会话表移除。
    void removeSession(struct tcp_pcb *pcb);
    // UDP 伪会话结束时从 UDP 会话表移除（按四元组 key）。仅 NetStack 线程调用。
    void removeUdpSession(const std::string &key);

    Client *getClient();

private:
    int initStack();
    void teardownStack();
    void loop();
    void handleInput(std::string packet);
    void feedToLwip(const std::string &innerPacket, IP4 vnetPeer);
    // UDP 伪会话空闲超时回收（仅 NetStack 线程，loop 中节流调用）。
    void reapIdleUdpSessions();

    // lwIP 回调跳板
    static err_t netifInitTrampoline(struct netif *netif);
    static err_t outputTrampoline(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr);
    static err_t acceptTrampoline(void *arg, struct tcp_pcb *newpcb, err_t err);
    static void udpRecvTrampoline(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);

    err_t onNetifInit(struct netif *netif);
    err_t onOutput(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr);
    err_t onAccept(struct tcp_pcb *newpcb, err_t err);
    // UDP 数据报到达：addr/port=源端(dev1)，pcb->local=目的(落地服务)。仅 NetStack 线程。
    void onUdpRecv(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);

private:
    Client *client;

    std::thread stackThread;
    std::atomic<bool> running;

    struct netif lwipNetif;
    struct tcp_pcb *listenPcb;
    // UDP 捕获所有目的的伪监听 pcb（绑定 netif、bind(NULL,0)，收首包克隆出 connect 的 npcb）。
    struct udp_pcb *udpListenPcb;

    Reactor reactor;

    // 落地出站：DirectOutbound（内核 socket 直连）。userspace 局域网组网的落地方式。
    DirectOutbound directOutbound;

    // NetStack 线程任务队列（用带超时读驱动定时器）
    std::mutex stackTaskMutex;
    std::condition_variable stackTaskCond;
    std::queue<std::function<void()>> stackTasks;

    // 内层源 IP(dev1) -> vnetPeer(源网关虚拟IP)，供回包寻址。
    // 带最近活跃时间戳，惰性老化清理（180s 空闲过期），避免长期运行无界累积。
    struct PeerEntry {
        IP4 peer;
        std::chrono::steady_clock::time_point lastSeen;
    };
    std::mutex peerMapMutex;
    std::unordered_map<uint32_t, PeerEntry> vnetPeerMap;

    std::mutex sessionMutex;
    std::unordered_map<struct tcp_pcb *, std::shared_ptr<SessionTcp>> sessions;
    // UDP 伪会话表：四元组 key -> SessionUdp。仅 NetStack 线程访问，无需加锁。
    std::unordered_map<std::string, std::shared_ptr<SessionUdp>> udpSessions;
};

} // namespace candy

#endif
