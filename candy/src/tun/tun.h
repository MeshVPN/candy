// SPDX-License-Identifier: MIT
#ifndef CANDY_TUN_TUN_H
#define CANDY_TUN_TUN_H

#include "core/message.h"
#include "core/net.h"
#include <any>
#include <chrono>
#include <cstdint>
#include <list>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace candy {

class Client;

// 发起流五元组：用于区分"本网关发起的流"(其返程应 L3 转发回本地 LAN)
// 与"新入站连接"(应在本落地端用 lwIP 终结)。仅 userspace 模式下使用。
struct FlowKey {
    uint32_t src;
    uint32_t dst;
    uint16_t sport;
    uint16_t dport;
    uint8_t proto;

    bool operator==(const FlowKey &o) const {
        return src == o.src && dst == o.dst && sport == o.sport && dport == o.dport && proto == o.proto;
    }
};

struct FlowKeyHash {
    std::size_t operator()(const FlowKey &k) const noexcept {
        std::size_t h = std::hash<uint32_t>{}(k.src);
        h ^= std::hash<uint32_t>{}(k.dst) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}((uint32_t(k.sport) << 16) | k.dport) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.proto) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

class Tun {
public:
    Tun();
    ~Tun();

    int setName(const std::string &name);
    int setMTU(int mtu);

    int run(Client *client);
    int wait();

    IP4 getIP();

private:
    int setAddress(const std::string &cidr);

    // 处理来自 TUN 设备的数据
    int handleTunDevice();

    // 处理来自消息队列的数据
    int handleTunQueue();
    int handlePacket(Msg msg);
    int handleTunAddr(Msg msg);
    int handleSysRt(Msg msg);

    std::string tunAddress;
    std::thread tunThread;
    std::thread msgThread;

private:
    int up();
    int down();

    int read(std::string &buffer);
    int write(const std::string &buffer);

    int setSysRtTable(const SysRouteEntry &entry);
    int setSysRtTable(IP4 dst, IP4 mask, IP4 nexthop);

    std::shared_mutex sysRtMutex;
    std::list<SysRouteEntry> sysRtTable;

    // userspace 模式发起流跟踪：记录本网关发起(本地 LAN -> 虚拟网)的流，
    // 用于判定收到的 IPIP 内层包是"返程"(L3 转发回本地 LAN)还是
    // "新入站连接"(落地端 lwIP 终结)。
    void trackOutboundFlow(const std::string &innerPacket);
    bool isReturnFlow(const std::string &innerPacket);

    std::mutex flowMutex;
    std::unordered_map<FlowKey, std::chrono::steady_clock::time_point, FlowKeyHash> flowTable;

private:
    std::any impl;

private:
    Client &getClient();
    Client *client;
};

} // namespace candy

#endif
