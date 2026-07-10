// SPDX-License-Identifier: MIT
#ifndef CANDY_TUN_TUN_H
#define CANDY_TUN_TUN_H

#include "core/message.h"
#include "core/net.h"
#include <any>
#include <chrono>
#include <cstdint>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace candy {

class Client;

// 发起流五元组：唯一标识一条「本机 LAN 主动发起、经本网关封 IPIP 转发」的流。
// 各字段按网络字节序原样存储（记录与查询表示一致即可，无需 ntoh 转换）。
struct FlowKey {
    uint8_t proto; // 6=TCP, 17=UDP
    uint32_t srcIP;
    uint16_t srcPort;
    uint32_t dstIP;
    uint16_t dstPort;
    bool operator==(const FlowKey &o) const {
        return proto == o.proto && srcIP == o.srcIP && srcPort == o.srcPort && dstIP == o.dstIP && dstPort == o.dstPort;
    }
};

// net.h 只提供了 hash<IP4>，FlowKey 需自定义 hash。
struct FlowKeyHash {
    std::size_t operator()(const FlowKey &k) const {
        std::size_t h = std::hash<uint32_t>{}(k.srcIP);
        h ^= std::hash<uint32_t>{}(k.dstIP) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}((uint32_t(k.srcPort) << 16) | k.dstPort) + 0x9e3779b9 + (h << 6) + (h >> 2);
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
    int getMTU() const;

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
    // IPIP 内层源是否落在本机路由子网内（返程/中转流量判定）。非 TCP/UDP（如 ICMP）
    // 场景下作为 isReturnByFlow 的回退启发式使用。
    bool isReturnTraffic(const IP4Header &header, std::size_t size);
    // 基于发起流跟踪表的返程判定：TCP/UDP 用反向五元组精确匹配，其余回退 isReturnTraffic。
    bool isReturnByFlow(const IP4Header &header, std::size_t size);
    // 从一个 IPv4 包解析 TCP/UDP 五元组（含分片/越界校验）；成功填 out 返回 true。
    bool parseInnerL4(const uint8_t *ipPkt, std::size_t len, FlowKey &out);
    // 发起端封 IPIP 前记录本机 LAN 主动发起的正向流（仅 TCP/UDP）。
    void recordInitiatedFlow(const std::string &origPacket);
    // 老化清理：剔除空闲超时的发起流，并对表容量兜底。
    void reapIdleFlows();
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

    // 发起流跟踪表：记录本机 LAN 主动发起、经本网关封 IPIP 转发的正向流。
    // 落地端据此用反向五元组区分「别人发起的新入站」(喂 lwIP) 与「本机发起流的返程」
    // (走内核)。tunThread 写、msgThread 读，用独立 mutex 保护（临界区仅 hash 操作，
    // 不得与 sysRtMutex 嵌套持有以防死锁）。
    std::mutex flowMutex;
    std::unordered_map<FlowKey, std::chrono::steady_clock::time_point, FlowKeyHash> initiatedFlows;
    std::chrono::steady_clock::time_point lastFlowReap;

private:
    std::any impl;

private:
    Client &getClient();
    Client *client;
};

} // namespace candy

#endif
