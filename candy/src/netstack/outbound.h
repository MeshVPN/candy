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

// Outbound：出站抽象。把"一条已被本机 lwIP 终结的流如何落地拨号"从 Session 中解耦。
//
// 当前增量发版仅保留 userspace 局域网组网所需的内核 socket 直连落地（DirectOutbound），
// socks5/mesh 等出站方式待后续迭代再引入。
//
// 设计约束（与现有 Session 实现保持一致）：
//   - dialTcp/dialUdp 只负责"建 socket + 设非阻塞 + 发起 connect"，返回一个已发起
//     连接的非阻塞 fd（失败返回 -1）。后续 reactor 注册、可读可写事件、双向 splice、
//     背压等全部仍由 Session 自身处理。
class Outbound {
public:
    virtual ~Outbound() = default;

    // 出站名称（direct）。
    virtual std::string name() const = 0;

    // L4 终结型出站：为一条已终结的 TCP 流建立落地通道，返回已发起 connect 的非阻塞
    // fd（EINPROGRESS 视为成功），失败返回 -1。默认不支持。
    virtual int dialTcp(const Endpoint &dst) {
        return -1;
    }

    // L4 终结型出站：为一条已终结的 UDP 流建立 connect 到目的的非阻塞 fd，失败返回 -1。
    // 默认不支持。
    virtual int dialUdp(const Endpoint &dst) {
        return -1;
    }
};

// DirectOutbound：内核 socket 直连落地。
// 源地址由内核在发起连接时自动填为本网关出口 IP，等价 MASQUERADE。
class DirectOutbound : public Outbound {
public:
    std::string name() const override {
        return "direct";
    }

    int dialTcp(const Endpoint &dst) override;
    int dialUdp(const Endpoint &dst) override;
};

} // namespace candy

#endif
