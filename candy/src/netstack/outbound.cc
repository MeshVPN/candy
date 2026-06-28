// SPDX-License-Identifier: MIT
#include "netstack/outbound.h"
#include "netstack/sockcompat.h"
#include "netstack/socks5_client.h"
#include <spdlog/spdlog.h>

#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#elif defined(_WIN32) || defined(_WIN64)
#include <ws2tcpip.h>
#endif

namespace candy {

namespace {
// 共享的 TCP 非阻塞拨号：建 socket + 非阻塞 + 发起 connect 到 dst。
// EINPROGRESS 视为成功，返回已发起 connect 的 fd；失败返回 -1。
// direct 与 socks5 都用它（区别仅在 dst 是业务目的还是 socks5 server）。
int dialTcpTo(const Endpoint &dst, const char *tag) {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
    int fd = (int)::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        spdlog::warn("{} tcp socket failed: {}", tag, netErrStr(netLastError()));
        return -1;
    }
    netSetNonBlocking(fd);

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dst.port);
    addr.sin_addr.s_addr = uint32_t(dst.host);

    int ret = ::connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret != 0 && !netInProgress(netLastError())) {
        spdlog::warn("{} tcp connect {}:{} failed: {}", tag, dst.host.toString(), dst.port, netErrStr(netLastError()));
        netClose(fd);
        return -1;
    }
    return fd;
#else
    return -1;
#endif
}
} // namespace

int DirectOutbound::dialTcp(const Endpoint &dst) {
    return dialTcpTo(dst, "direct outbound");
}

int DirectOutbound::dialUdp(const Endpoint &dst) {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
    int fd = (int)::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        spdlog::warn("direct outbound udp socket failed: {}", netErrStr(netLastError()));
        return -1;
    }
    netSetNonBlocking(fd);

    // connect 固定对端：内核自动填源地址 = 网关 LAN IP（等价 MASQUERADE），
    // 且后续 send/recv 只与该对端往来，形成 Fullcone NAT 的伪会话。
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dst.port);
    addr.sin_addr.s_addr = uint32_t(dst.host);
    if (::connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        spdlog::warn("direct outbound udp connect {}:{} failed: {}", dst.host.toString(), dst.port,
                     netErrStr(netLastError()));
        netClose(fd);
        return -1;
    }
    return fd;
#else
    return -1;
#endif
}

Socks5Outbound::Socks5Outbound(Endpoint server, std::string username, std::string password)
    : server(server), username(std::move(username)), password(std::move(password)) {}

int Socks5Outbound::dialTcp(const Endpoint &dst) {
    // 注意：socks5 拨号连接的是 server，而非业务目的 dst（dst 留给握手阶段 CONNECT）。
    (void)dst;
    return dialTcpTo(this->server, "socks5 outbound");
}

std::unique_ptr<TcpHandshake> Socks5Outbound::makeTcpHandshake(const Endpoint &dst) {
    auto hs = std::make_unique<Socks5Client>();
    hs->startConnectIPv4(dst.host, dst.port, this->username, this->password);
    return hs;
}

int Socks5Outbound::dialUdpControl() {
    // UDP ASSOCIATE 的控制连接：仍是到 socks5 server 的 TCP 连接（与 dialTcp 同目标）。
    return dialTcpTo(this->server, "socks5 udp control");
}

std::unique_ptr<TcpHandshake> Socks5Outbound::makeUdpAssociate() {
    auto hs = std::make_unique<Socks5Client>();
    hs->startUdpAssociate(this->username, this->password);
    return hs;
}

} // namespace candy
