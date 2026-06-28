// SPDX-License-Identifier: MIT
#ifndef CANDY_NETSTACK_OUTBOUND_H
#define CANDY_NETSTACK_OUTBOUND_H

#include "core/net.h"
#include "netstack/tcp_handshake.h"
#include <cstdint>
#include <memory>
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

    // 为本出站的一条 TCP 流创建"连接建立后、splice 之前"的应用层握手器。
    // direct/mesh 无需握手，返回 nullptr（Session 据此走原有直连 splice 逻辑，字节不变）；
    // socks5 返回一个已按目的地址初始化的 Socks5Client。
    //   dst：业务真实目的（socks5 CONNECT 的目标，而非 socks5 server 地址）。
    virtual std::unique_ptr<TcpHandshake> makeTcpHandshake(const Endpoint &dst) { return nullptr; }

    // UDP 是否需要先在 TCP 控制连接上做 UDP ASSOCIATE 关联：direct 为 false（内核 socket
    // 直连即可）；socks5 为 true（需先建控制连接握手、拿到中继端点，再经中继收发封装报文）。
    virtual bool udpNeedsAssociation() const { return false; }

    // socks5 UDP 关联用：拨号到 socks5 server 的 TCP 控制连接（与 dialTcp 同一目标 server，
    // 但语义独立）。返回已发起 connect 的非阻塞 fd，失败返回 -1。默认不支持。
    virtual int dialUdpControl() { return -1; }

    // socks5 UDP 关联用：创建在控制连接上跑 UDP ASSOCIATE 命令的握手器（complete 后由
    // udpRelayHost()/udpRelayPort() 取中继端点）。默认返回 nullptr。
    virtual std::unique_ptr<TcpHandshake> makeUdpAssociate() { return nullptr; }

    // socks5 UDP 关联用：代理服务端端点。当 UDP ASSOCIATE 应答的中继地址为 0.0.0.0 时，
    // 按 RFC 1928 §7 回退到控制连接的服务端 IP。非 socks5 出站返回 0.0.0.0:0（不使用）。
    virtual Endpoint proxyServerEndpoint() const { return Endpoint{IP4("0.0.0.0"), 0}; }
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

// Socks5Outbound：外部 socks5 代理落地。dialTcp 连接的是 socks5 server（而非业务目的），
// 连上后由 makeTcpHandshake 返回的 Socks5Client 在通道上完成 CONNECT 握手，
// 之后 Session 把 lwIP pcb <-> socks5 连接做双向 splice。needsTermination()==true。
// 协议限制：不支持 ICMP；UDP（UDP ASSOCIATE）后续单独实现，此处先支持 TCP。
class Socks5Outbound : public Outbound {
public:
    // server：socks5 服务端地址；username/password 为空表示无认证。
    Socks5Outbound(Endpoint server, std::string username = "", std::string password = "");

    std::string name() const override { return "socks5"; }
    bool needsTermination() const override { return true; }

    // 连接 socks5 server（而非业务目的 dst）。
    int dialTcp(const Endpoint &dst) override;

    // 返回针对业务目的 dst 的 socks5 CONNECT 握手器。
    std::unique_ptr<TcpHandshake> makeTcpHandshake(const Endpoint &dst) override;

    // UDP 走 UDP ASSOCIATE：需要先建 TCP 控制连接并握手拿到中继端点。
    bool udpNeedsAssociation() const override { return true; }
    int dialUdpControl() override;
    std::unique_ptr<TcpHandshake> makeUdpAssociate() override;
    Endpoint proxyServerEndpoint() const override { return this->server; }

private:
    Endpoint server;
    std::string username;
    std::string password;
};

} // namespace candy

#endif
