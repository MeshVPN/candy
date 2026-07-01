// SPDX-License-Identifier: MIT
#ifndef CANDY_NETSTACK_SESSION_H
#define CANDY_NETSTACK_SESSION_H

#include <atomic>
#include <cstdint>

namespace candy {

class NetStack;

// Session 基类：会话生命周期的公共抽象，派生出 TCP/UDP 两类会话。
class Session {
public:
    explicit Session(NetStack *stack);
    virtual ~Session() = default;

    // 启动落地：建立内核 socket、发起连接、注册到 reactor。
    virtual int start() = 0;

protected:
    NetStack *stack;
    std::atomic<bool> closing;
};

} // namespace candy

#endif
