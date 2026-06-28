// SPDX-License-Identifier: MIT
#ifndef CANDY_NETSTACK_SOCKS5_CLIENT_H
#define CANDY_NETSTACK_SOCKS5_CLIENT_H

#include "core/net.h"
#include "netstack/tcp_handshake.h"
#include <cstdint>
#include <string>
#include <vector>

namespace candy {

// socks5 协议常量（RFC 1928 / 1929，取值与 hev-socks5-proto.h 对齐）。
namespace Socks5 {
constexpr uint8_t VERSION = 0x05;          // socks 版本号
constexpr uint8_t AUTH_VERSION = 0x01;     // user/pass 认证子协商版本（RFC 1929）
constexpr uint8_t METHOD_NONE = 0x00;      // 无认证
constexpr uint8_t METHOD_USERPASS = 0x02;  // 用户名口令认证
constexpr uint8_t METHOD_NOACCEPT = 0xFF;  // 服务端拒绝所有方法
constexpr uint8_t CMD_CONNECT = 0x01;      // CONNECT 请求
constexpr uint8_t CMD_UDP_ASSOCIATE = 0x03;// UDP ASSOCIATE 请求
constexpr uint8_t ATYP_IPV4 = 0x01;        // 地址类型：IPv4
constexpr uint8_t ATYP_DOMAIN = 0x03;      // 地址类型：域名
constexpr uint8_t ATYP_IPV6 = 0x04;        // 地址类型：IPv6
constexpr uint8_t REP_SUCCESS = 0x00;      // 应答：成功
} // namespace Socks5

// Socks5Client：非阻塞 reactor 模型下的 socks5 CONNECT 握手增量状态机。
//
// 设计要点（适配 candy 的 reactor，区别于 hev 的协程阻塞 IO）：
//   - 本类不持有 fd、不直接收发；只做"协议字节"的生成与解析。
//   - 调用方在 socket 可写时取 takeOutbound() 的待发送字节并 send；
//     在 socket 可读时把收到的字节 feed() 进来推进状态机。
//   - 每次推进后通过 state() 查询：是否还要发/收、是否完成(Done)、是否失败(Failed)。
//   - 完成后，余下未消费的入站字节（握手应答之后紧跟的业务数据，极少见但协议允许）
//     可由 takeLeftover() 取走，交给后续 splice。
//
// 握手时序（标准 CONNECT，参照 hev-socks5-client.c handshake_standard）：
//   WriteAuthMethods -> ReadAuthMethod
//     -> [若选中 USERPASS] WriteAuthCreds -> ReadAuthCreds
//     -> WriteRequest -> ReadResponse -> Done
class Socks5Client : public TcpHandshake {
public:
    enum class State {
        Init,             // 未开始
        WriteAuthMethods, // 待发送：版本+认证方法列表
        ReadAuthMethod,   // 待接收：服务端选定的认证方法
        WriteAuthCreds,   // 待发送：用户名/口令
        ReadAuthCreds,    // 待接收：认证结果
        WriteRequest,     // 待发送：CONNECT 请求
        ReadResponse,     // 待接收：CONNECT 应答
        Done,             // 握手成功
        Failed,           // 握手失败
    };

    // 目标地址用 IPv4 直发（ATYP=IPv4）。creds 为空表示仅声明无认证方法。
    void startConnectIPv4(IP4 dstHost, uint16_t dstPort, const std::string &username = "",
                          const std::string &password = "");

    // 目标地址用域名直发（ATYP=DOMAIN），由 socks5 服务端解析。
    void startConnectDomain(const std::string &domain, uint16_t dstPort, const std::string &username = "",
                            const std::string &password = "");

    // UDP ASSOCIATE 握手（RFC 1928 §7）：在 TCP 控制连接上请求 UDP 中继。
    // 请求里的 DST.ADDR/DST.PORT 填 0.0.0.0:0（表示客户端中继源端口未定，服务端不限制）。
    // 完成后由 udpRelayHost()/udpRelayPort() 取服务端返回的 UDP 中继端点（BND.ADDR:BND.PORT）。
    void startUdpAssociate(const std::string &username = "", const std::string &password = "");

    // 喂入从 socket 收到的字节，推进状态机。返回 false 表示握手失败（state()==Failed）。
    bool feed(const uint8_t *data, size_t len) override;

    // 取走当前待发送字节（取走后内部清空）。调用方负责把它写到 socket；
    // 若一次没写完，剩余部分应由调用方自行保留续发，本类不重复给出。
    std::vector<uint8_t> takeOutbound() override;

    // 是否还有待发送字节。
    bool hasOutbound() const override;

    // 握手完成后，取走应答之后多余的入站业务数据（通常为空）。
    std::vector<uint8_t> takeLeftover() override;

    State state() const;
    bool done() const override;
    bool failed() const override;
    const std::string &error() const override;

    IP4 udpRelayHost() const override;
    uint16_t udpRelayPort() const override;

private:
    void buildAuthMethods();
    void buildAuthCreds();
    void buildRequest();
    void fail(const std::string &reason);

    State st = State::Init;
    bool useUserPass = false;
    std::string username;
    std::string password;

    // 握手命令：CONNECT(TCP) 或 UDP ASSOCIATE(UDP 中继)，决定 buildRequest 的 CMD 与
    // 应答里 BND.ADDR/BND.PORT 的语义（CONNECT 时无用，UDP ASSOCIATE 时为中继端点）。
    uint8_t command = Socks5::CMD_CONNECT;

    // 目标地址（二选一：IPv4 或域名）。
    bool useDomain = false;
    IP4 dstHost;
    std::string dstDomain;
    uint16_t dstPort = 0;

    // UDP ASSOCIATE 应答解析出的中继端点（仅 command==UDP ASSOCIATE 且 Done 后有效）。
    IP4 relayAddr;
    uint16_t relayPortValue = 0;

    std::vector<uint8_t> outbound;  // 待发送缓冲
    std::vector<uint8_t> inbound;   // 入站累积缓冲（用于跨次拼接定长/变长应答）
    std::vector<uint8_t> leftover;  // 握手完成后多余的入站字节
    std::string errMsg;             // 失败原因（仅在 Failed 时有效）
};

// socks5 UDP 中继数据报封装（RFC 1928 §7）。每个发往中继的 UDP 数据报前缀一个头：
//   RSV(2=0x0000) FRAG(1=0x00) ATYP(1) DST.ADDR DST.PORT DATA。
// 本工程 IPv4-only，ATYP 固定 0x01。返回"头 + 业务数据"的完整数据报字节。
std::string socks5UdpEncap(IP4 dstHost, uint16_t dstPort, const uint8_t *data, size_t len);

// socks5 UDP 中继数据报解封装：解析中继回包的头，输出原始目的端点(srcHost/srcPort)
// 与业务数据。返回 false 表示包格式非法或 FRAG!=0（不支持分片，按 RFC 丢弃）。
// 成功时 payload 指向 pkt 内部的业务数据起点（零拷贝），payloadLen 为其长度。
bool socks5UdpDecap(const uint8_t *pkt, size_t len, IP4 &srcHost, uint16_t &srcPort, const uint8_t **payload,
                    size_t &payloadLen);

} // namespace candy

#endif
