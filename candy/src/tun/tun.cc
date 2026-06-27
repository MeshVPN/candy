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

    // userspace 模式：记录本网关发起(本地 LAN -> 虚拟网)的 TCP 流，
    // 以便收到 IPIP 返程包时识别并 L3 转发回本地 LAN，而非误终结。
    if (getClient().getForwardMode() == "userspace") {
        trackOutboundFlow(buffer.substr(sizeof(IP4Header)));
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

    // userspace 模式：落地端把"新入站连接"的 IPIP 整包交给 NetStack 终结（参照1），
    // 由 NetStack 记录 vnetPeer 并剥 IPIP、喂 lwIP；不写内核 tun。
    // 但若该 IPIP 内层是本网关"发起流"的返程（如 SYN-ACK），本网关是发起端，
    // 必须剥 IPIP 后写内核 tun，L3 转发回本地 LAN，而非误喂自己的 lwIP。
    // kernel 模式：保持现状，剥 IPIP 外层后写内核 tun。
    if (getClient().getForwardMode() == "userspace" && header->isIPIP()) {
        std::string inner = msg.data.substr(sizeof(IP4Header));
        if (!isReturnFlow(inner)) {
            getClient().getNetStack().input(std::move(msg.data));
            return 0;
        }
        write(inner);
        return 0;
    }

    if (header->isIPIP()) {
        msg.data.erase(0, sizeof(IP4Header));
        header = (IP4Header *)msg.data.data();
    }
    write(msg.data);
    return 0;
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

// 仅跟踪 TCP（阶段一）。返回是否成功解析出五元组。
static bool parseFlow(const std::string &inner, FlowKey &key) {
    if (inner.size() < sizeof(IP4Header)) {
        return false;
    }
    const IP4Header *ip = (const IP4Header *)inner.data();
    if (ip->protocol != 0x06) { // TCP
        return false;
    }
    size_t ihl = (ip->version_ihl & 0x0f) * 4;
    if (inner.size() < ihl + 4) {
        return false;
    }
    const uint8_t *l4 = (const uint8_t *)inner.data() + ihl;
    key.src = uint32_t(ip->saddr);
    key.dst = uint32_t(ip->daddr);
    key.sport = ((uint16_t)l4[0] << 8) | l4[1];
    key.dport = ((uint16_t)l4[2] << 8) | l4[3];
    key.proto = ip->protocol;
    return true;
}

void Tun::trackOutboundFlow(const std::string &innerPacket) {
    FlowKey key;
    if (!parseFlow(innerPacket, key)) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    std::unique_lock lock(this->flowMutex);
    this->flowTable[key] = now;
    // 顺带做惰性老化清理，避免表无限增长（180s 空闲过期）。
    if (this->flowTable.size() > 4096) {
        for (auto it = this->flowTable.begin(); it != this->flowTable.end();) {
            if (now - it->second > std::chrono::seconds(180)) {
                it = this->flowTable.erase(it);
            } else {
                ++it;
            }
        }
    }
}

bool Tun::isReturnFlow(const std::string &innerPacket) {
    FlowKey in;
    if (!parseFlow(innerPacket, in)) {
        return false;
    }
    // 返程包的五元组是发起流的反向：src/dst、sport/dport 互换。
    FlowKey rev{in.dst, in.src, in.dport, in.sport, in.proto};
    std::unique_lock lock(this->flowMutex);
    auto it = this->flowTable.find(rev);
    if (it == this->flowTable.end()) {
        return false;
    }
    it->second = std::chrono::steady_clock::now();
    return true;
}

} // namespace candy
