// SPDX-License-Identifier: MIT
#include "netstack/netstack.h"
#include "core/client.h"
#include "core/message.h"
#include "netstack/session_tcp.h"
#include "netstack/session_udp.h"
#include <chrono>
#include <cstring>
#include <mutex>
#include <spdlog/spdlog.h>
#include <vector>

#include "lwip/init.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "netif/etharp.h"

namespace candy {

NetStack::NetStack() : client(nullptr), running(false), listenPcb(nullptr), udpListenPcb(nullptr) {
    std::memset(&this->lwipNetif, 0, sizeof(this->lwipNetif));
}

NetStack::~NetStack() {
    shutdown();
    wait();
}

Client *NetStack::getClient() {
    return this->client;
}

Reactor &NetStack::getReactor() {
    return this->reactor;
}

Outbound &NetStack::getOutbound() {
    // 当前仅支持内核 socket 直连落地。
    return this->directOutbound;
}

int NetStack::run(Client *client) {
    this->client = client;
    this->running.store(true);

    if (this->reactor.start()) {
        spdlog::critical("netstack reactor start failed");
        return -1;
    }

    this->stackThread = std::thread([this] {
        spdlog::debug("start thread: netstack");
        try {
            if (initStack()) {
                spdlog::critical("netstack init failed");
                getClient()->shutdown();
            } else {
                loop();
                teardownStack();
            }
        } catch (const std::exception &e) {
            spdlog::error("netstack thread exception: {}", e.what());
            getClient()->shutdown();
        }
        spdlog::debug("stop thread: netstack");
    });
    return 0;
}

int NetStack::wait() {
    if (this->stackThread.joinable()) {
        this->stackThread.join();
    }
    this->reactor.stop();
    return 0;
}

void NetStack::shutdown() {
    this->running.store(false);
    this->stackTaskCond.notify_all();
}

void NetStack::input(std::string packet) {
    if (!this->running.load()) {
        return;
    }
    postToStack([this, packet = std::move(packet)]() mutable { handleInput(std::move(packet)); });
}

void NetStack::postToStack(std::function<void()> task) {
    {
        std::unique_lock lock(this->stackTaskMutex);
        this->stackTasks.push(std::move(task));
    }
    this->stackTaskCond.notify_one();
}

int NetStack::initStack() {
    static std::once_flag lwipInitFlag;
    std::call_once(lwipInitFlag, [] { lwip_init(); });

    ip4_addr_t addr, mask, gw;
    IP4_ADDR(&addr, 10, 255, 255, 254);
    IP4_ADDR(&mask, 255, 255, 255, 255);
    ip4_addr_set_any(&gw);

    if (netif_add(&this->lwipNetif, &addr, &mask, &gw, this, netifInitTrampoline, ip_input) == nullptr) {
        spdlog::critical("netstack netif_add failed");
        return -1;
    }
    netif_set_up(&this->lwipNetif);
    netif_set_link_up(&this->lwipNetif);
    netif_set_default(&this->lwipNetif);
    netif_set_flags(&this->lwipNetif, NETIF_FLAG_PRETEND_TCP);

    // netif MTU 必须扣除 IPIP 外层头(20B)，否则落地端按 1500 协商出的满载内层包
    // 封装后超过隧道 tun MTU(client mtu)，导致大包被丢弃、TCP 重传超时。
    int tunMtu = this->client ? this->client->getMtu() : 1400;
    int netifMtu = tunMtu - (int)sizeof(IP4Header);
    if (netifMtu < 576) {
        netifMtu = 576;
    }
    this->lwipNetif.mtu = (u16_t)netifMtu;

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb == nullptr) {
        spdlog::critical("netstack tcp_new failed");
        return -1;
    }
    tcp_bind_netif(pcb, &this->lwipNetif);
    if (tcp_bind(pcb, nullptr, 0) != ERR_OK) {
        spdlog::critical("netstack tcp_bind failed");
        return -1;
    }
    this->listenPcb = tcp_listen(pcb);
    if (this->listenPcb == nullptr) {
        spdlog::critical("netstack tcp_listen failed");
        return -1;
    }
    tcp_arg(this->listenPcb, this);
    tcp_accept(this->listenPcb, acceptTrampoline);

    // UDP 捕获所有目的：建一个绑定本 netif 的 udp_pcb，bind(NULL,0) 接管任意目的。
    // 依赖 netif 的 NETIF_FLAG_PRETEND_TCP：收到首个数据报时 lwIP 会克隆出一个
    // 已 connect 源端的 npcb 并通过回调交给我们，由此建立四元组伪会话。
    this->udpListenPcb = udp_new_ip_type(IPADDR_TYPE_ANY);
    if (this->udpListenPcb == nullptr) {
        spdlog::critical("netstack udp_new failed");
        return -1;
    }
    udp_bind_netif(this->udpListenPcb, &this->lwipNetif);
    if (udp_bind(this->udpListenPcb, nullptr, 0) != ERR_OK) {
        spdlog::critical("netstack udp_bind failed");
        return -1;
    }
    udp_recv(this->udpListenPcb, udpRecvTrampoline, this);
    return 0;
}

void NetStack::teardownStack() {
    // lwIP 进程级单例：重连时必须清理本次注册的 netif / pcb / 会话，
    // 否则 netif 会在全局 netif_list 中累积，最终触发 "too many netifs" 断言崩溃。
    {
        std::unique_lock lock(this->sessionMutex);
        for (auto &kv : this->sessions) {
            if (kv.second) {
                kv.second->shutdownFromStack();
            }
        }
        this->sessions.clear();
    }
    // UDP 伪会话仅 NetStack 线程访问，此处即在 NetStack 线程，直接清理。
    for (auto &kv : this->udpSessions) {
        if (kv.second) {
            kv.second->shutdownFromStack();
        }
    }
    this->udpSessions.clear();
    {
        std::unique_lock lock(this->peerMapMutex);
        this->vnetPeerMap.clear();
    }
    if (this->udpListenPcb != nullptr) {
        udp_recv(this->udpListenPcb, nullptr, nullptr);
        udp_remove(this->udpListenPcb);
        this->udpListenPcb = nullptr;
    }
    if (this->listenPcb != nullptr) {
        tcp_close(this->listenPcb);
        this->listenPcb = nullptr;
    }
    netif_remove(&this->lwipNetif);
    std::memset(&this->lwipNetif, 0, sizeof(this->lwipNetif));
}

void NetStack::loop() {
    auto lastReap = std::chrono::steady_clock::now();
    while (this->running.load()) {
        std::function<void()> task;
        {
            std::unique_lock lock(this->stackTaskMutex);
            this->stackTaskCond.wait_for(lock, std::chrono::milliseconds(250),
                                         [this] { return !this->stackTasks.empty() || !this->running.load(); });
            if (!this->stackTasks.empty()) {
                task = std::move(this->stackTasks.front());
                this->stackTasks.pop();
            }
        }
        if (task) {
            task();
        }
        sys_check_timeouts();

        // 节流：每 10s 扫描一次 UDP 伪会话，回收空闲超时项。
        auto now = std::chrono::steady_clock::now();
        if (now - lastReap >= std::chrono::seconds(10)) {
            reapIdleUdpSessions();
            lastReap = now;
        }
    }
}

void NetStack::reapIdleUdpSessions() {
    // 仅 NetStack 线程访问 udpSessions，无需加锁。
    // UDP 无连接：60s 无收发则判定空闲，强制关闭并移除（shutdownFromStack 会
    // 移除 npcb 并交由 reactor 关闭 fd）。注意 shutdownFromStack 内不修改 udpSessions，
    // 故可安全地在遍历后统一 erase。
    auto now = std::chrono::steady_clock::now();
    int reaped = 0;
    for (auto it = this->udpSessions.begin(); it != this->udpSessions.end();) {
        if (it->second && now - it->second->lastActive() > std::chrono::seconds(60)) {
            it->second->shutdownFromStack();
            it = this->udpSessions.erase(it);
            ++reaped;
        } else {
            ++it;
        }
    }
    if (reaped > 0) {
        spdlog::debug("netstack reap idle udp sessions: {} reaped, {} remain", reaped, this->udpSessions.size());
    }
}

void NetStack::handleInput(std::string packet) {
    if (packet.size() < sizeof(IP4Header)) {
        return;
    }
    IP4Header *header = (IP4Header *)packet.data();
    IP4 vnetPeer;
    if (header->isIPIP()) {
        vnetPeer = header->saddr;
        packet.erase(0, sizeof(IP4Header));
    }
    if (packet.size() < sizeof(IP4Header)) {
        return;
    }
    IP4Header *inner = (IP4Header *)packet.data();
    spdlog::debug("netstack input: vnetPeer={} {} -> {} proto={}", vnetPeer.toString(), inner->saddr.toString(),
                  inner->daddr.toString(), (int)inner->protocol);
    // 本次增量发版仅支持 userspace TCP/UDP 组网：非 TCP(0x06)/UDP(0x11) 一律丢弃
    // （ICMP 等待后续迭代再支持；PRETEND netif 本也只接受 TCP/UDP）。
    if (inner->protocol != 0x06 && inner->protocol != 0x11) {
        return;
    }
    feedToLwip(packet, vnetPeer);
}

void NetStack::feedToLwip(const std::string &innerPacket, IP4 vnetPeer) {
    IP4Header *header = (IP4Header *)innerPacket.data();
    if (!vnetPeer.empty()) {
        auto now = std::chrono::steady_clock::now();
        std::unique_lock lock(this->peerMapMutex);
        this->vnetPeerMap[uint32_t(header->saddr)] = PeerEntry{vnetPeer, now};
        // 惰性老化清理：表过大时清掉 180s 空闲过期项，避免长期运行无界累积。
        if (this->vnetPeerMap.size() > 4096) {
            for (auto it = this->vnetPeerMap.begin(); it != this->vnetPeerMap.end();) {
                if (now - it->second.lastSeen > std::chrono::seconds(180)) {
                    it = this->vnetPeerMap.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    struct pbuf *p = pbuf_alloc(PBUF_RAW, innerPacket.size(), PBUF_RAM);
    if (p == nullptr) {
        spdlog::warn("netstack pbuf_alloc failed");
        return;
    }
    pbuf_take(p, innerPacket.data(), innerPacket.size());
    err_t e = this->lwipNetif.input(p, &this->lwipNetif);
    if (e != ERR_OK) {
        spdlog::warn("netstack netif.input failed: {}", (int)e);
        pbuf_free(p);
    }
}

void NetStack::output(const std::string &innerPacket) {
    if (innerPacket.size() < sizeof(IP4Header)) {
        return;
    }
    IP4Header *inner = (IP4Header *)innerPacket.data();
    IP4 vnetPeer;
    {
        std::unique_lock lock(this->peerMapMutex);
        auto it = this->vnetPeerMap.find(uint32_t(inner->daddr));
        if (it != this->vnetPeerMap.end()) {
            vnetPeer = it->second.peer;
            it->second.lastSeen = std::chrono::steady_clock::now();
        }
    }
    if (vnetPeer.empty()) {
        spdlog::warn("netstack output drop: no vnetPeer for {}", inner->daddr.toString());
        return;
    }

    std::string buffer = innerPacket;
    buffer.insert(0, sizeof(IP4Header), 0);
    IP4Header *outer = (IP4Header *)buffer.data();
    outer->protocol = 0x04;
    outer->saddr = this->client->address();
    outer->daddr = vnetPeer;

    this->client->getPeerMsgQueue().write(Msg(MsgKind::PACKET, std::move(buffer)));
}

void NetStack::removeSession(struct tcp_pcb *pcb) {
    std::unique_lock lock(this->sessionMutex);
    this->sessions.erase(pcb);
}

void NetStack::removeUdpSession(const std::string &key) {
    // 仅 NetStack 线程调用，udpSessions 无需加锁。
    this->udpSessions.erase(key);
}

struct netif &NetStack::getNetif() {
    return this->lwipNetif;
}

err_t NetStack::netifInitTrampoline(struct netif *netif) {
    NetStack *self = (NetStack *)netif->state;
    return self->onNetifInit(netif);
}

err_t NetStack::outputTrampoline(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr) {
    NetStack *self = (NetStack *)netif->state;
    return self->onOutput(netif, p, ipaddr);
}

err_t NetStack::acceptTrampoline(void *arg, struct tcp_pcb *newpcb, err_t err) {
    NetStack *self = (NetStack *)arg;
    return self->onAccept(newpcb, err);
}

void NetStack::udpRecvTrampoline(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    NetStack *self = (NetStack *)arg;
    self->onUdpRecv(pcb, p, addr, port);
}

err_t NetStack::onNetifInit(struct netif *netif) {
    netif->name[0] = 'c';
    netif->name[1] = 'd';
    netif->mtu = 1500;
    netif->output = outputTrampoline;
    return ERR_OK;
}

err_t NetStack::onOutput(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr) {
    std::string buffer;
    buffer.resize(p->tot_len);
    pbuf_copy_partial(p, buffer.data(), p->tot_len, 0);
    if (buffer.size() >= sizeof(IP4Header)) {
        IP4Header *h = (IP4Header *)buffer.data();
        spdlog::debug("netstack onOutput: {} -> {} proto={} len={}", h->saddr.toString(), h->daddr.toString(),
                      (int)h->protocol, buffer.size());
    }
    output(buffer);
    return ERR_OK;
}

err_t NetStack::onAccept(struct tcp_pcb *newpcb, err_t err) {
    spdlog::debug("netstack onAccept: err={} pcb={}", (int)err, (void *)newpcb);
    if (err != ERR_OK || newpcb == nullptr) {
        return ERR_VAL;
    }
    if (!this->running.load()) {
        return ERR_RST;
    }

    auto session = std::make_shared<SessionTcp>(this, newpcb);
    if (session->start()) {
        return ERR_RST;
    }
    {
        std::unique_lock lock(this->sessionMutex);
        this->sessions[newpcb] = session;
    }
    return ERR_OK;
}

void NetStack::onUdpRecv(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    // 首包路径：lwIP 已为该流克隆出已 connect 的 npcb（pcb 参数），
    //   addr/port = 目的(dev2 服务)，pcb->remote_ip/remote_port = 源端(dev1)。
    // 我们建立伪会话并在 npcb 上注册每流 recv handler，然后**不释放 p**：
    //   lwIP 收尾会 goto again 把同一数据报按已连接 npcb 重投，交由每流 handler 处理。
    if (p == nullptr) {
        return;
    }
    if (!this->running.load()) {
        udp_remove(pcb);
        pbuf_free(p);
        return;
    }

    IP4 origDst;
    std::memcpy(&origDst, &ip_2_ip4(addr)->addr, sizeof(uint32_t));
    uint16_t origDstPort = port;
    IP4 origSrc;
    std::memcpy(&origSrc, &ip_2_ip4(&pcb->remote_ip)->addr, sizeof(uint32_t));
    uint16_t origSrcPort = pcb->remote_port;

    spdlog::debug("netstack onUdpRecv: {}:{} -> {}:{}", origSrc.toString(), origSrcPort, origDst.toString(),
                  origDstPort);

    auto session = std::make_shared<SessionUdp>(this, pcb, origSrc, origSrcPort, origDst, origDstPort);
    if (session->start()) {
        udp_recv(pcb, nullptr, nullptr);
        udp_remove(pcb);
        pbuf_free(p);
        return;
    }
    this->udpSessions[session->key()] = session;
    // 不释放 p：交还 lwIP（goto again 重投到已连接 npcb 的每流 handler）。
}

} // namespace candy
