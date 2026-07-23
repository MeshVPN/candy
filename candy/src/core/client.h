// SPDX-License-Identifier: MIT
#ifndef CANDY_CORE_CLIENT_H
#define CANDY_CORE_CLIENT_H

#include "core/message.h"
#include "peer/manager.h"
#include "tun/tun.h"
#include "utils/atomic.h"
#include "websocket/client.h"
#ifdef CANDY_NETSTACK
#include "netstack/netstack.h"
#endif
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>

namespace candy {

class MsgQueue {
public:
    Msg read();
    void write(Msg msg);
    void clear();

private:
    std::queue<Msg> msgQueue;
    std::mutex msgMutex;
    std::condition_variable msgCondition;
};

class Client {
public:
    void setName(const std::string &name);
    void setPassword(const std::string &password);
    void setWebSocket(const std::string &uri);
    void setTunAddress(const std::string &cidr);
    void setStun(const std::string &stun);
    void setDiscoveryInterval(int interval);
    void setRouteCost(int cost);
    void setPort(int port);
    void setLocalhost(std::string ip);
    void setMtu(int mtu);

    void setExptTunAddress(const std::string &cidr);
    void setVirtualMac(const std::string &vmac);

    void setP2POnly(bool enable);
    void setUserspaceStack(bool enable);
    void setUdpPortConvergence(bool enable);

    void run();
    bool isRunning();
    void shutdown();

    std::string getName() const;
    std::string getTunCidr() const;
    IP4 address();

private:
    Utils::Atomic<bool> running;

public:
    MsgQueue &getTunMsgQueue();
    MsgQueue &getPeerMsgQueue();
    MsgQueue &getWsMsgQueue();
    MsgQueue &getNetStackMsgQueue();

    bool getP2POnly() const;
    bool getUserspaceStack() const;
    bool getUdpPortConvergence() const;
    int getMtu() const;

private:
    MsgQueue tunMsgQueue, peerMsgQueue, wsMsgQueue, netstackMsgQueue;

    Tun tun;
    PeerManager peerManager;
    WebSocketClient ws;
#ifdef CANDY_NETSTACK
    NetStack netstack;
#endif

private:
    std::string tunName;
    bool p2pOnly = false;
    bool userspaceStack = false;
    // UDP 单端口收敛开关（默认关闭＝每源全锥形）。仅在 userspaceStack=true 时生效。
    bool udpPortConvergence = false;
};

} // namespace candy

#endif
