// SPDX-License-Identifier: MIT
#include "netstack/session_icmp.h"
#include "netstack/netstack.h"
#include <cstring>
#include <spdlog/spdlog.h>

#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace candy {

namespace {

// 计算 16 位反码和校验（IP/ICMP 通用）。data 长度可奇可偶。
uint16_t checksum16(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint16_t)((data[0] << 8) | data[1]);
        data += 2;
        len -= 2;
    }
    if (len > 0) {
        sum += (uint16_t)(data[0] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

constexpr size_t ICMP_HDR_LEN = 8; // type+code+checksum+id+seq

} // namespace

SessionIcmp::SessionIcmp(NetStack *stack, IP4 origSrc, IP4 origDst, uint16_t icmpId)
    : Session(stack), fd(-1), rawMode(false), origSrc(origSrc), origDst(origDst), icmpId(icmpId),
      lastActiveTs(std::chrono::steady_clock::now()) {
    // key：源IP + 目的IP + icmp id（同一 ping 进程对同一目的复用一个落地 fd）。
    this->sessionKey.assign((const char *)&origSrc, sizeof(uint32_t));
    this->sessionKey.append((const char *)&origDst, sizeof(uint32_t));
    this->sessionKey.append((const char *)&icmpId, sizeof(icmpId));
}

SessionIcmp::~SessionIcmp() {
    if (this->fd >= 0) {
        ::close(this->fd);
        this->fd = -1;
    }
}

int SessionIcmp::start() {
#if defined(__linux__) || defined(__APPLE__)
    this->self = shared_from_this();

    // 优先非特权 DGRAM ICMP socket（需 ping_group_range 覆盖运行用户）；
    // 失败退化为 RAW ICMP socket（需 root / CAP_NET_RAW）。
    this->fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (this->fd < 0) {
        this->fd = ::socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        this->rawMode = true;
    }
    if (this->fd < 0) {
        spdlog::warn("session icmp socket failed: {}", strerror(errno));
        return -1;
    }
    int flags = ::fcntl(this->fd, F_GETFL, 0);
    ::fcntl(this->fd, F_SETFL, flags | O_NONBLOCK);

    // connect 固定对端：内核自动填源地址 = 网关 LAN IP（等价 MASQUERADE）。
    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = uint32_t(this->origDst);
    if (::connect(this->fd, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        spdlog::warn("session icmp connect {} failed: {}", this->origDst.toString(), strerror(errno));
        ::close(this->fd);
        this->fd = -1;
        return -1;
    }

    spdlog::debug("session icmp: {} -> {} id={} {}", this->origSrc.toString(), this->origDst.toString(), this->icmpId,
                  this->rawMode ? "raw" : "dgram");

    auto holder = shared_from_this();
    int fd = this->fd;
    this->stack->getReactor().add(fd, ReactorEvent::READ, [holder](ReactorEvent ev) { holder->onFdEvent((uint32_t)ev); });
    return 0;
#else
    return -1;
#endif
}

// ===================== NetStack 线程 =====================

void SessionIcmp::sendEcho(std::string icmpData) {
    // 仅 NetStack 线程调用：刷新活跃时间，把 echo request 报文投递到 reactor 发送。
    // icmpData 是完整 ICMP 报文（type/code/checksum/id/seq/payload）。
    // DGRAM ICMP socket：内核会忽略并改写 id/checksum，按 fd 配对回包。
    this->lastActiveTs = std::chrono::steady_clock::now();
    auto holder = shared_from_this();
    this->stack->getReactor().post([holder, data = std::move(icmpData)]() mutable {
#if defined(__linux__) || defined(__APPLE__)
        if (holder->fd < 0 || holder->closing.load()) {
            return;
        }
        ssize_t n = ::send(holder->fd, data.data(), data.size(), 0);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            spdlog::debug("session icmp send failed: {}", strerror(errno));
        }
#endif
    });
}

void SessionIcmp::replyToPeer(std::string icmpReply) {
    // 仅 NetStack 线程调用：icmpReply 是落地 fd 收到的 echo reply 的 ICMP 报文。
    // DGRAM ICMP socket 回包里 id 是内核分配值，需还原成源端 origIcmpId；
    // 然后重算 ICMP 校验和，再手工封 IP 头(源=origDst, 目的=origSrc)，
    // 交 NetStack::output 封 IPIP 回源端。
    if (icmpReply.size() < ICMP_HDR_LEN) {
        return;
    }
    this->lastActiveTs = std::chrono::steady_clock::now();

    uint8_t *icmp = (uint8_t *)icmpReply.data();
    // 还原 icmp id（偏移 4..5），并把 type 规整为 echo reply(0)。
    icmp[0] = 0; // echo reply
    icmp[1] = 0; // code
    icmp[4] = (uint8_t)(this->icmpId >> 8);
    icmp[5] = (uint8_t)(this->icmpId & 0xff);
    // 重算 ICMP 校验和（偏移 2..3 先清零）。
    icmp[2] = 0;
    icmp[3] = 0;
    uint16_t csum = checksum16(icmp, icmpReply.size());
    icmp[2] = (uint8_t)(csum >> 8);
    icmp[3] = (uint8_t)(csum & 0xff);

    // 手工封 IP 头：源=origDst(dev2)，目的=origSrc(dev1)，协议=ICMP(1)。
    std::string packet;
    packet.resize(sizeof(IP4Header) + icmpReply.size());
    IP4Header *ip = (IP4Header *)packet.data();
    std::memset(ip, 0, sizeof(IP4Header));
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->tot_len = hton((uint16_t)(sizeof(IP4Header) + icmpReply.size()));
    ip->id = 0;
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = 0x01; // ICMP
    ip->check = 0;
    ip->saddr = this->origDst;
    ip->daddr = this->origSrc;
    uint16_t ipsum = checksum16((const uint8_t *)ip, sizeof(IP4Header));
    ip->check = hton(ipsum);

    std::memcpy(packet.data() + sizeof(IP4Header), icmpReply.data(), icmpReply.size());

    // 经 NetStack::output 按 origSrc 查 vnetPeer、封 IPIP 回源端。
    this->stack->output(packet);
}

void SessionIcmp::shutdownFromStack() {
    // 栈拆除/空闲回收：fd 交由 reactor 关闭并释放自持。
    if (!this->closing.exchange(true)) {
        auto holder = shared_from_this();
        this->stack->getReactor().post([holder] {
#if defined(__linux__) || defined(__APPLE__)
            if (holder->fd >= 0) {
                holder->stack->getReactor().del(holder->fd);
                ::close(holder->fd);
                holder->fd = -1;
            }
#endif
            holder->self.reset();
        });
    }
}

// ===================== Reactor 线程 =====================

void SessionIcmp::onFdEvent(uint32_t events) {
    ReactorEvent ev = (ReactorEvent)events;
    if (ev & ReactorEvent::ERROR) {
        closeFromReactor();
        return;
    }
    if (ev & ReactorEvent::READ) {
        onFdReadable();
    }
}

void SessionIcmp::onFdReadable() {
#if defined(__linux__) || defined(__APPLE__)
    char buf[65536];
    while (true) {
        ssize_t n = ::recv(this->fd, buf, sizeof(buf), 0);
        if (n > 0) {
            size_t off = 0;
            // RAW ICMP socket 收到的是含 IP 头的整包，需剥掉 IP 头取 ICMP；
            // DGRAM ICMP socket 收到的直接是 ICMP 报文。
            if (this->rawMode && n >= (ssize_t)sizeof(IP4Header)) {
                uint8_t ihl = (((uint8_t *)buf)[0] & 0x0f) * 4;
                if ((ssize_t)ihl <= n) {
                    off = ihl;
                }
            }
            std::string data(buf + off, n - off);
            auto holder = shared_from_this();
            this->stack->postToStack([holder, data = std::move(data)]() mutable { holder->replyToPeer(std::move(data)); });
            continue;
        }
        if (n == 0) {
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        // 落地 socket 错误（如目的不可达）：关闭会话。
        closeFromReactor();
        return;
    }
#endif
}

void SessionIcmp::closeFromReactor() {
    if (this->closing.exchange(true)) {
        return;
    }
#if defined(__linux__) || defined(__APPLE__)
    if (this->fd >= 0) {
        this->stack->getReactor().del(this->fd);
        ::close(this->fd);
        this->fd = -1;
    }
#endif
    NetStack *stack = this->stack;
    std::string k = this->sessionKey;
    auto holder = shared_from_this();
    stack->postToStack([holder, stack, k] {
        stack->removeIcmpSession(k);
        holder->self.reset();
    });
}

} // namespace candy
