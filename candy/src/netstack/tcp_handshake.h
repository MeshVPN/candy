// SPDX-License-Identifier: MIT
#ifndef CANDY_NETSTACK_TCP_HANDSHAKE_H
#define CANDY_NETSTACK_TCP_HANDSHAKE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/net.h"

namespace candy {

// TcpHandshake：在落地 TCP 连接建立之后、进入双向 splice 之前，需要先完成的
// 应用层握手抽象（如 socks5 CONNECT）。direct 出站无需握手（makeTcpHandshake 返回 nullptr）。
//
// 由 Reactor 线程单线程驱动，区别于 hev 的协程阻塞 IO：
//   - 可写时：取 takeOutbound() 的待发字节写入 fd；
//   - 可读时：把收到的字节 feed() 进来推进；
//   - done() 表示握手成功，failed() 表示失败，takeLeftover() 取握手后粘连的业务数据。
// Session 由此把"握手如何进行"与"具体协议(socks5)"解耦。
class TcpHandshake {
public:
    virtual ~TcpHandshake() = default;

    // 取走当前待发送字节（取走后内部清空），调用方负责写入 fd。
    virtual std::vector<uint8_t> takeOutbound() = 0;
    // 是否还有待发送字节。
    virtual bool hasOutbound() const = 0;
    // 喂入从 fd 收到的字节推进握手；返回 false 表示握手失败。
    virtual bool feed(const uint8_t *data, size_t len) = 0;
    // 握手是否成功完成。
    virtual bool done() const = 0;
    // 握手是否失败。
    virtual bool failed() const = 0;
    // 握手完成后取走应答之后多余的入站业务数据（通常为空）。
    virtual std::vector<uint8_t> takeLeftover() = 0;
    // 失败原因（仅在 failed() 时有效）。
    virtual const std::string &error() const = 0;

    // UDP ASSOCIATE 专用：握手完成后服务端返回的 UDP 中继端点。
    // 仅 socks5 UDP 关联握手有意义；其余握手返回默认值（调用方不会使用）。
    virtual IP4 udpRelayHost() const { return IP4("0.0.0.0"); }
    virtual uint16_t udpRelayPort() const { return 0; }
};

} // namespace candy

#endif
