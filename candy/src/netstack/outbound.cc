// SPDX-License-Identifier: MIT
#include "netstack/outbound.h"
#include "netstack/sockcompat.h"
#include <spdlog/spdlog.h>

// 说明（回应 review：优先用 Poco 封装）：此处属「不得已」而使用平台相关裸 socket。
// 原因：落地 fd 的生命周期由 Reactor 按裸 int fd 统一管理（注册/注销/关闭），
// 而 Poco::Net::StreamSocket/DatagramSocket 会自行持有并在析构时关闭 fd，
// 与 Reactor 的 fd 所有权模型冲突，若混用会导致 double-close。故这里只借用
// 系统 socket() 拿到裸 fd 交给 Reactor，平台差异已收敛在 sockcompat.h 内，
// 本文件仅剩 sockaddr_in 组装这一处必须用平台头。
#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#elif defined(_WIN32) || defined(_WIN64)
#include <ws2tcpip.h>
#endif

namespace candy {

int DirectOutbound::dialTcp(const Endpoint &dst) {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
    // 建 socket + 非阻塞 + 发起 connect 到 dst。EINPROGRESS 视为成功。
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
        spdlog::warn("direct outbound tcp connect {}:{} failed: {}", dst.host.toString(), dst.port, netErrStr(netLastError()));
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
        spdlog::warn("direct outbound udp connect {}:{} failed: {}", dst.host.toString(), dst.port, netErrStr(netLastError()));
        netClose(fd);
        return -1;
    }
    return fd;
#else
    return -1;
#endif
}

} // namespace candy
