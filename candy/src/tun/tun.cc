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
    if (header->isIPIP() && !isReturnTraffic(*header, msg.data.size()) && getClient().getUserspaceStack()) {
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
