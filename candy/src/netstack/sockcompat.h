// SPDX-License-Identifier: MIT
#ifndef CANDY_NETSTACK_SOCKCOMPAT_H
#define CANDY_NETSTACK_SOCKCOMPAT_H

// 落地 socket 的跨平台薄封装：抹平 POSIX(Linux/macOS) 与 Winsock(Windows) 的差异，
// 使 session_tcp / session_udp 的落地收发逻辑三端共用同一份代码。
// 约定：对外统一以 int 表示 socket（Windows SOCKET 为小整数句柄，转换安全）。

#include <string>
#include <system_error>

#if defined(_WIN32) || defined(_WIN64)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h> // htons/ntohs（无连接收发的端口字节序转换）
#include <cerrno>      // errno 是 C 标准库唯一入口，无 C++ 等价，属不得已保留。
#include <fcntl.h>
#include <netinet/in.h> // struct sockaddr_in / INADDR_ANY（无连接收发地址组装）
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace candy {

// 关闭落地 socket。
inline void netClose(int fd) {
#if defined(_WIN32) || defined(_WIN64)
    ::closesocket((SOCKET)fd);
#else
    ::close(fd);
#endif
}

// 设为非阻塞。成功返回 0。
inline int netSetNonBlocking(int fd) {
#if defined(_WIN32) || defined(_WIN64)
    u_long on = 1;
    return ::ioctlsocket((SOCKET)fd, FIONBIO, &on) == 0 ? 0 : -1;
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

// 收：返回读到字节数；0 表示对端关闭；<0 表示错误（含 would-block）。
inline long netRecv(int fd, void *buf, size_t len) {
#if defined(_WIN32) || defined(_WIN64)
    return ::recv((SOCKET)fd, (char *)buf, (int)len, 0);
#else
    return ::recv(fd, buf, len, 0);
#endif
}

// 发：返回写出字节数；<0 表示错误（含 would-block）。
inline long netSend(int fd, const void *buf, size_t len) {
#if defined(_WIN32) || defined(_WIN64)
    return ::send((SOCKET)fd, (const char *)buf, (int)len, 0);
#else
    return ::send(fd, buf, len, 0);
#endif
}

// 无连接发（全锥形出口用）：目的地址(网络序 s_addr) + 目的端口(主机序)。
// 返回写出字节数；<0 表示错误（含 would-block）。
inline long netSendTo(int fd, const void *buf, size_t len, uint32_t dstIpBe, uint16_t dstPortHost) {
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dstPortHost);
    addr.sin_addr.s_addr = dstIpBe;
#if defined(_WIN32) || defined(_WIN64)
    return ::sendto((SOCKET)fd, (const char *)buf, (int)len, 0, (struct sockaddr *)&addr, sizeof(addr));
#else
    return ::sendto(fd, buf, len, 0, (struct sockaddr *)&addr, sizeof(addr));
#endif
}

// 无连接收（全锥形出口用）：返回读到字节数；0 表示无数据；<0 表示错误（含 would-block）。
// 同时回填对端地址(网络序 s_addr)与端口(主机序)。
inline long netRecvFrom(int fd, void *buf, size_t len, uint32_t *outIpBe, uint16_t *outPortHost) {
    struct sockaddr_in addr = {};
#if defined(_WIN32) || defined(_WIN64)
    int alen = (int)sizeof(addr);
    long n = ::recvfrom((SOCKET)fd, (char *)buf, (int)len, 0, (struct sockaddr *)&addr, &alen);
#else
    socklen_t alen = sizeof(addr);
    long n = ::recvfrom(fd, buf, len, 0, (struct sockaddr *)&addr, &alen);
#endif
    if (n >= 0) {
        if (outIpBe != nullptr) {
            *outIpBe = addr.sin_addr.s_addr;
        }
        if (outPortHost != nullptr) {
            *outPortHost = ntohs(addr.sin_port);
        }
    }
    return n;
}

// 绑定到 (INADDR_ANY, 0)：让内核立即分配一个固定出口端口，全生命周期内不变，
// 对所有目的复用同一 (出口IP,端口)，实现全锥形 endpoint-independent mapping。成功返回 0。
inline int netBindAny(int fd) {
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = INADDR_ANY;
#if defined(_WIN32) || defined(_WIN64)
    return ::bind((SOCKET)fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 ? 0 : -1;
#else
    return ::bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 ? 0 : -1;
#endif
}

// 调大接收缓冲区（SO_RCVBUF）。单端口收敛时单个 fd 要收全部对端回包，
// 突发海量流可能溢出默认缓冲导致丢包（原多 socket 天然分摊，无此问题）。成功返回 0。
inline int netSetRecvBuf(int fd, int bytes) {
#if defined(_WIN32) || defined(_WIN64)
    return ::setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVBUF, (const char *)&bytes, sizeof(bytes)) == 0 ? 0 : -1;
#else
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) == 0 ? 0 : -1;
#endif
}

// 取本线程最近一次 socket 错误码。
inline int netLastError() {
#if defined(_WIN32) || defined(_WIN64)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

// 错误码是否为"暂时不可用"（非阻塞下应重试，而非关闭）。
inline bool netWouldBlock(int err) {
#if defined(_WIN32) || defined(_WIN64)
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

// 错误码是否为"连接进行中"（非阻塞 connect 的正常返回）。
inline bool netInProgress(int err) {
#if defined(_WIN32) || defined(_WIN64)
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return err == EINPROGRESS;
#endif
}

// 错误码转可读字符串（仅用于日志）。
inline std::string netErrStr(int err) {
#if defined(_WIN32) || defined(_WIN64)
    return "wsa error " + std::to_string(err);
#else
    return std::generic_category().message(err);
#endif
}

} // namespace candy

#endif
