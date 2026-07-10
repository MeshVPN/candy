// SPDX-License-Identifier: MIT
#include "tun/tun.h"
#include "core/client.h"
#include "core/message.h"
#include "core/net.h"
#include "utils/hex.h"
#include "utils/log.h"
#include <Poco/Format.h>
#include <mutex>
#include <shared_mutex>

namespace candy {

int Tun::run(Client *client) {
    this->client = client;
    this->msgThread = std::thread([&] {
        candy::logger().debug("start thread: tun msg");
        try {
            while (getClient().isRunning()) {
                if (handleTunQueue()) {
                    break;
                }
            }
            getClient().shutdown();
        } catch (const std::exception &e) {
            candy::logger().error(Poco::format("tun msg thread exception: %s", std::string(e.what())));
            getClient().shutdown();
        }
        candy::logger().debug("stop thread: tun msg");
    });
    return 0;
}

int Tun::wait() {
    if (this->tunThread.joinable()) {
        this->tunThread.join();
    }
    if (this->msgThread.joinable()) {
        this->msgThread.join();
    }
    {
        std::unique_lock lock(this->sysRtMutex);
        this->sysRtTable.clear();
    }
    return 0;
}

int Tun::handleTunDevice() {
    std::string buffer;
    int error = read(buffer);
    if (error <= 0) {
        return 0;
    }
    if (buffer.length() < sizeof(IP4Header)) {
        return 0;
    }
    IP4Header *header = (IP4Header *)buffer.data();
    if (!header->isIPv4()) {
        return 0;
    }

    IP4 nextHop = [&]() {
        std::shared_lock lock(this->sysRtMutex);
        for (auto const &rt : sysRtTable) {
            if ((header->daddr & rt.mask) == rt.dst) {
                return rt.nexthop;
            }
        }
        return IP4();
    }();
    if (!nextHop.empty()) {
#ifdef CANDY_NETSTACK
        // 发起端仅在 userspace 模式记录正向流：内核模式做 SNAT 且不喂 lwIP，无需跟踪，
        // 门控可省纯 kernel 网关的内存/CPU。落地端据此表用反向五元组区分新入站/返程。
        // 必须在 insert 封 IPIP 前记录：此时 buffer 仍是原始内层单层包。
        if (getClient().getUserspaceStack()) {
            recordInitiatedFlow(buffer);
        }
#endif
        buffer.insert(0, sizeof(IP4Header), 0);
        header = (IP4Header *)buffer.data();
        header->protocol = 0x04;
        header->saddr = getIP();
        header->daddr = nextHop;
    }

    if (header->daddr == getIP()) {
        write(buffer);
        return 0;
    }

    this->client->getPeerMsgQueue().write(Msg(MsgKind::PACKET, std::move(buffer)));
    return 0;
}

int Tun::handleTunQueue() {
    Msg msg = this->client->getTunMsgQueue().read();
    switch (msg.kind) {
    case MsgKind::TIMEOUT:
        break;
    case MsgKind::PACKET:
        handlePacket(std::move(msg));
        break;
    case MsgKind::TUNADDR:
        if (handleTunAddr(std::move(msg))) {
            return -1;
        }
        break;
    case MsgKind::SYSRT:
        handleSysRt(std::move(msg));
        break;
    default:
        candy::logger().warning(Poco::format("unexcepted tun message type: %d", static_cast<int>(msg.kind)));
        break;
    }
    return 0;
}

int Tun::handlePacket(Msg msg) {
    if (msg.data.size() < sizeof(IP4Header)) {
        candy::logger().warning(Poco::format("invalid IPv4 packet: %s", to_hex(msg.data)));
        return 0;
    }
    IP4Header *header = (IP4Header *)msg.data.data();

#ifdef CANDY_NETSTACK
    // userspace 模式下的新入站连接：内层源不可路由的 IPIP 包整包交给 NetStack 用 lwIP
    // 终结。这一分支不剥壳、不写内核 tun，直接经 MsgQueue 转交 netstack 模块（跨模块
    // 数据一律走 MsgQueue，不直接持有 netstack）。判定完成后提前返回，下方只保留唯一
    // 的一处 erase + write。未编译 CANDY_NETSTACK 时本分支不存在，一律走内核转发。
    if (header->isIPIP() && !isReturnByFlow(*header, msg.data.size()) && getClient().getUserspaceStack()) {
        this->client->getNetStackMsgQueue().write(Msg(MsgKind::NETSTACK, std::move(msg.data)));
        return 0;
    }
#endif

    // 其余情况都要写内核 tun：
    // - IPIP 返程/中转，或 kernel 模式的 IPIP 入站：剥掉 IPIP 外层(20B)后写。
    // - 非 IPIP 的普通包：原样写。
    if (header->isIPIP()) {
        msg.data.erase(0, sizeof(IP4Header));
    }
    write(msg.data);
    return 0;
}

bool Tun::isReturnTraffic(const IP4Header &header, size_t size) {
    // 内层源地址落在本机已知路由子网内 => 本机(经本网关)发起流量的返程/中转。
    // 复用本就为路由维护的 sysRtTable：「本机能发起到的子网」== 「sysRtTable 命中的
    // 子网」，O(1) 空间，无需额外发起流跟踪表。
    if (size < sizeof(IP4Header) * 2) {
        return false;
    }
    const IP4Header *inner = &header + 1;
    std::shared_lock lock(this->sysRtMutex);
    for (auto const &rt : sysRtTable) {
        if ((inner->saddr & rt.mask) == rt.dst) {
            return true;
        }
    }
    return false;
}

bool Tun::parseInnerL4(const uint8_t *ipPkt, std::size_t len, FlowKey &out) {
    // 从一个裸 IPv4 包解析 TCP/UDP 五元组。所有偏移访问前都做长度校验防越界。
    if (len < sizeof(IP4Header)) {
        return false;
    }
    const IP4Header *ip = (const IP4Header *)ipPkt;
    if ((ip->version_ihl >> 4) != 4) {
        return false;
    }
    // 仅跟踪 TCP(6)/UDP(17)：其余(如 ICMP)交回退启发式处理。
    if (ip->protocol != 0x06 && ip->protocol != 0x11) {
        return false;
    }
    // 分片：非首片(offset!=0)没有 L4 头，按 IHL 偏移读到的是 payload，会构造出垃圾键，
    // 导致同一数据报首片/余片判定不一致、重组崩坏。一律跳过(返回 false 回退)。
    uint16_t fragOff = ntoh(ip->frag_off);
    if ((fragOff & 0x1FFF) != 0) {
        return false;
    }
    std::size_t ihl = (ip->version_ihl & 0x0F) * 4;
    if (ihl < sizeof(IP4Header)) {
        return false;
    }
    // 源/目的端口都在 L4 头前 4 字节，需保证 IP 头 + 4 字节可读。
    if (len < ihl + 4) {
        return false;
    }
    const uint8_t *l4 = ipPkt + ihl;
    uint16_t srcPort, dstPort;
    std::memcpy(&srcPort, l4, sizeof(uint16_t));
    std::memcpy(&dstPort, l4 + 2, sizeof(uint16_t));
    // 端口按网络字节序原样存(记录与查询表示一致即可，无需 ntoh)。
    out.proto = ip->protocol;
    out.srcIP = ip->saddr;
    out.srcPort = srcPort;
    out.dstIP = ip->daddr;
    out.dstPort = dstPort;
    return true;
}

void Tun::recordInitiatedFlow(const std::string &origPacket) {
    // 发起端：本机 LAN 主动发起、经本网关封 IPIP 转发的正向流。按内层五元组记录，
    // 返程回来时落地端用反向五元组匹配到本条 => 判返程走内核。
    FlowKey key;
    if (!parseInnerL4((const uint8_t *)origPacket.data(), origPacket.size(), key)) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    std::unique_lock lock(this->flowMutex);
    this->initiatedFlows[key] = now;
    reapIdleFlows();
}

bool Tun::isReturnByFlow(const IP4Header &header, size_t size) {
    // 落地端：区分「别人主动发起的新入站」(喂 lwIP) 与「本机发起流的返程」(走内核)。
    if (size < sizeof(IP4Header) * 2) {
        return false;
    }
    const uint8_t *inner = (const uint8_t *)(&header + 1);
    std::size_t innerLen = size - sizeof(IP4Header);
    FlowKey fwd;
    if (!parseInnerL4(inner, innerLen, fwd)) {
        // 非 TCP/UDP(如 ICMP)或分片：无法做五元组精确匹配，回退旧的子网启发式，
        // 保持 ICMP 等现状零回归。
        return isReturnTraffic(header, size);
    }
    // 用入站包的反向五元组还原发起方向：入站是 peer->self，发起过则表里有 self->peer。
    FlowKey rev;
    rev.proto = fwd.proto;
    rev.srcIP = fwd.dstIP;
    rev.srcPort = fwd.dstPort;
    rev.dstIP = fwd.srcIP;
    rev.dstPort = fwd.srcPort;
    std::unique_lock lock(this->flowMutex);
    auto it = this->initiatedFlows.find(rev);
    if (it != this->initiatedFlows.end()) {
        // 命中：本机之前发起过该流，此入站包是其返程；刷新时间戳(长连接保活)。
        it->second = std::chrono::steady_clock::now();
        return true;
    }
    return false;
}

void Tun::reapIdleFlows() {
    // 调用方已持有 flowMutex。节流：每 ~10s 才真正扫描一次，避免每包遍历。
    auto now = std::chrono::steady_clock::now();
    bool overCapacity = this->initiatedFlows.size() > 4096;
    if (!overCapacity && now - this->lastFlowReap < std::chrono::seconds(10)) {
        return;
    }
    this->lastFlowReap = now;
    // 空闲 > 300s 的流剔除：返程通常秒级到达，长连接的正向包会持续刷新时间戳。
    for (auto it = this->initiatedFlows.begin(); it != this->initiatedFlows.end();) {
        if (now - it->second > std::chrono::seconds(300)) {
            it = this->initiatedFlows.erase(it);
        } else {
            ++it;
        }
    }
    // 容量兜底：清理过期项后仍超限(如 LAN 设备扫描/伪造多目的)，淘汰最旧项防内存爆炸。
    while (this->initiatedFlows.size() > 4096) {
        auto oldest = this->initiatedFlows.begin();
        for (auto it = this->initiatedFlows.begin(); it != this->initiatedFlows.end(); ++it) {
            if (it->second < oldest->second) {
                oldest = it;
            }
        }
        this->initiatedFlows.erase(oldest);
    }
}

int Tun::handleTunAddr(Msg msg) {
    if (setAddress(msg.data)) {
        return -1;
    }

    if (up()) {
        candy::logger().fatal("tun up failed");
        return -1;
    }

    this->tunThread = std::thread([&] {
        candy::logger().debug("start thread: tun");
        try {
            while (getClient().isRunning()) {
                if (handleTunDevice()) {
                    break;
                }
            }
            getClient().shutdown();
            candy::logger().debug("stop thread: tun");

            if (down()) {
                candy::logger().fatal("tun down failed");
                return;
            }
        } catch (const std::exception &e) {
            candy::logger().error(Poco::format("tun thread exception: %s", std::string(e.what())));
            getClient().shutdown();
        }
    });

    return 0;
}

int Tun::handleSysRt(Msg msg) {
    SysRouteEntry *rt = (SysRouteEntry *)msg.data.data();
    if (rt->nexthop != getIP()) {
        candy::logger().information(
            Poco::format("route: %s/%d via %s", rt->dst.toString(), rt->mask.toPrefix(), rt->nexthop.toString()));
        if (setSysRtTable(*rt)) {
            return -1;
        }
    }
    return 0;
}

int Tun::setSysRtTable(const SysRouteEntry &entry) {
    std::unique_lock lock(this->sysRtMutex);
    this->sysRtTable.push_back(entry);
    return setSysRtTable(entry.dst, entry.mask, entry.nexthop);
}

Client &Tun::getClient() {
    return *this->client;
}

} // namespace candy
