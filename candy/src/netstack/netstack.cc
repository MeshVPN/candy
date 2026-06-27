// SPDX-License-Identifier: MIT
#include "netstack/netstack.h"
#include "core/client.h"
#include "core/message.h"
#include "netstack/session_tcp.h"
#include <chrono>
#include <cstring>
#include <mutex>
#include <spdlog/spdlog.h>

#include "lwip/init.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/etharp.h"

namespace candy {

NetStack::NetStack() : client(nullptr), running(false), listenPcb(nullptr) {
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
    {
        std::unique_lock lock(this->peerMapMutex);
        this->vnetPeerMap.clear();
    }
    if (this->listenPcb != nullptr) {
        tcp_close(this->listenPcb);
        this->listenPcb = nullptr;
    }
    netif_remove(&this->lwipNetif);
    std::memset(&this->lwipNetif, 0, sizeof(this->lwipNetif));
}

void NetStack::loop() {
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

} // namespace candy
