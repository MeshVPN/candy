// SPDX-License-Identifier: MIT
#ifndef CANDY_CORE_CLIENT_H
#define CANDY_CORE_CLIENT_H

#include "core/message.h"
#include "netstack/netstack.h"
#include "peer/manager.h"
#include "tun/tun.h"
#include "utils/atomic.h"
#include "websocket/client.h"
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

    void setForwardMode(const std::string &mode);
    // 阶段三：外部 socks5 上游与分流规则（仅 userspace 模式生效，空表示全部 direct）。
    void setSocks5Upstream(const std::string &upstream);
    void setOutboundRules(const std::string &rules);

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

    NetStack &getNetStack();
    std::string getForwardMode() const;
    int getMtu() const;

private:
    MsgQueue tunMsgQueue, peerMsgQueue, wsMsgQueue;

    Tun tun;
    PeerManager peerManager;
    WebSocketClient ws;
    NetStack netstack;

private:
    std::string tunName;
    std::string forwardMode = "kernel";
    std::string socks5Upstream;
    std::string outboundRules;
    int mtu = 1400;
};

} // namespace candy

#endif
