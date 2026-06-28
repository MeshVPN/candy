// SPDX-License-Identifier: MIT
#include "netstack/outbound.h"
#include "netstack/sockcompat.h"
#include <spdlog/spdlog.h>

#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#elif defined(_WIN32) || defined(_WIN64)
#include <ws2tcpip.h>
#endif

namespace candy {

int DirectOutbound::dialTcp(const Endpoint &dst) {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
    int fd = (int)::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        spdlog::warn("direct outbound tcp socket failed: {}", netErrStr(netLastError()));
        return -1;
    }
    netSetNonBlocking(fd);

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dst.port);
    addr.sin_addr.s_addr = uint32_t(dst.host);

    int ret = ::connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret != 0 && !netInProgress(netLastError())) {
        spdlog::warn("direct outbound tcp connect {}:{} failed: {}", dst.host.toString(), dst.port,
                     netErrStr(netLastError()));
        netClose(fd);
        return -1;
    }
    return fd;
#else
    return -1;
#endif
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

} // namespace candy
