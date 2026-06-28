// SPDX-License-Identifier: MIT
#ifndef CANDY_NETSTACK_SOCKCOMPAT_H
#define CANDY_NETSTACK_SOCKCOMPAT_H

// 落地 socket 的跨平台薄封装：抹平 POSIX(Linux/macOS) 与 Winsock(Windows) 的差异，
// 使 session_tcp / session_udp 的落地收发逻辑三端共用同一份代码。
// 约定：对外统一以 int 表示 socket（Windows SOCKET 为小整数句柄，转换安全）。

#include <cstddef>
#include <string>

#if defined(_WIN32) || defined(_WIN64)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
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
    return std::strerror(err);
#endif
}

} // namespace candy

#endif
