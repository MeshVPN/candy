// SPDX-License-Identifier: MIT
#ifndef CANDY_NETSTACK_OUTBOUND_H
#define CANDY_NETSTACK_OUTBOUND_H

#include "core/net.h"
#include <cstdint>
#include <string>

namespace candy {

// 落地目的端点：一条被 lwIP 终结的流要连到的目标地址与端口。
struct Endpoint {
    IP4 host;
    uint16_t port;
};

// Outbound：出站抽象。把"一条已被本机 lwIP 终结的流如何落地拨号"从 Session 中解耦，
// 使落地方式可替换（阶段一/二的内核 socket 直连收敛为 DirectOutbound，阶段三再加
// Socks5Outbound / MeshOutbound）。
//
// 设计约束（承接 03 文档 §3，并与现有 Session 实现保持一致）：
//   - dialTcp/dialUdp 只负责"建 socket + 设非阻塞 + 发起 connect"，返回一个已发起
//     连接的非阻塞 fd（失败返回 -1）。后续 reactor 注册、可读可写事件、双向 splice、
//     背压等全部仍由 Session 自身处理 —— 因此本次抽取对运行时行为零影响。
//   - L4 终结型出站（direct/socks5）实现 dialTcp/dialUdp；needsTermination()==true。
//   - L3 转发型出站（mesh）不终结、走 forwardL3，dialTcp/dialUdp 返回 -1。
class Outbound {
public:
    virtual ~Outbound() = default;

    // 出站名称（direct / socks5 / mesh）。
    virtual std::string name() const = 0;

    // 是否需要发起端 lwIP 终结：direct/socks5 为 true，mesh 为 false。
    virtual bool needsTermination() const = 0;

    // L4 终结型出站：为一条已终结的 TCP 流建立落地通道，返回已发起 connect 的非阻塞
    // fd（EINPROGRESS 视为成功），失败返回 -1。默认不支持。
    virtual int dialTcp(const Endpoint &dst) { return -1; }

    // L4 终结型出站：为一条已终结的 UDP 流建立 connect 到目的的非阻塞 fd，失败返回 -1。
    // 默认不支持。
    virtual int dialUdp(const Endpoint &dst) { return -1; }
};

// DirectOutbound：内核 socket 直连落地（阶段一/二的拨号逻辑迁移至此）。
// 源地址由内核在发起连接时自动填为本网关出口 IP，等价 MASQUERADE。
class DirectOutbound : public Outbound {
public:
    std::string name() const override { return "direct"; }
    bool needsTermination() const override { return true; }

    int dialTcp(const Endpoint &dst) override;
    int dialUdp(const Endpoint &dst) override;
};

// MeshOutbound：L3 转发型出口（自建节点 mesh）。发起端不终结，按 L3 原样封 IPIP 经
// candy 虚拟网送到远端落地网关，由其在落地端用 lwIP 终结。保留 ICMP。
//
// 当前为阶段三骨架：现有 tun.cc::handlePacket 的 IPIP 封装/直转路径已实现等价行为，
// 此处先建立类型占位，待 Router 接入热路径后再把 forwardL3 与现有路径收敛。
class MeshOutbound : public Outbound {
public:
    std::string name() const override { return "mesh"; }
    bool needsTermination() const override { return false; }
    // L4 拨号对 mesh 无意义：mesh 不在发起端终结，dialTcp/dialUdp 保持默认返回 -1。
};

} // namespace candy

#endif
