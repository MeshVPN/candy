// SPDX-License-Identifier: MIT
#ifndef CANDY_NETSTACK_SESSION_TCP_H
#define CANDY_NETSTACK_SESSION_TCP_H

#include "core/net.h"
#include "netstack/session.h"
#include "netstack/tcp_handshake.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "lwip/tcp.h"

namespace candy {

// SessionTcp：一条被 lwIP 终结的 TCP 连接对应一个内核 socket 落地。
//
// 线程契约：
//   - 所有 tcp_*(pcb) 调用只在 NetStack 线程执行；
//   - 所有 fd 的 read/write 只在 Reactor 线程执行；
//   - 两侧通过 NetStack::postToStack / Reactor::post 投递任务跨线程交互。
//
// 数据流：
//   正向 lwIP->fd：tcp_recv 回调(NetStack线程) 收数据 -> 缓存 -> 投递 reactor 在可写时 write(fd)
//   反向 fd->lwIP：fd 可读(reactor线程) read -> 投递 NetStack 执行 tcp_write + tcp_output
class SessionTcp : public Session, public std::enable_shared_from_this<SessionTcp> {
public:
    SessionTcp(NetStack *stack, struct tcp_pcb *pcb);
    ~SessionTcp() override;

    int start() override;

    // 仅供 NetStack 线程在栈拆除（重连）时调用，强制关闭 pcb 与 fd。
    void shutdownFromStack();

private:
    // ---- NetStack 线程 ----
    static err_t recvTrampoline(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
    static err_t sentTrampoline(void *arg, struct tcp_pcb *pcb, u16_t len);
    static void errTrampoline(void *arg, err_t err);

    err_t onRecv(struct tcp_pcb *pcb, struct pbuf *p, err_t err);
    err_t onSent(struct tcp_pcb *pcb, u16_t len);
    void onErr(err_t err);

    void writeToLwip(const std::string &data);
    void pumpToLwip();
    void onFdEofFromStack();
    void closeFromStack();
    // 把已写进落地 fd 的字节数向 lwIP 确认（推进接收窗口）。仅 NetStack 线程调用。
    void ackRecvedToLwip();

    // ---- Reactor 线程 ----
    void onFdEvent(uint32_t events);
    void onConnected();
    void onFdReadable();
    void onFdWritable();
    void closeFromReactor();

    // 握手阶段（仅当 outbound 提供了 handshake 时，连接成功后进入；direct 无握手不进入）。
    // 在 Reactor 线程驱动：把待发字节写出、把收到字节喂入握手器，完成后切到 splice。
    void onHandshakeReadable();
    void onHandshakeWritable();
    void flushHandshakeOutbound();
    void finishHandshake();

    void flushForwardLocked();
    void updateReadInterest();

private:
    struct tcp_pcb *pcb;
    int fd;
    bool connected;

    // 应用层握手器（仅 socks5 等需要握手的 outbound 非空；direct 为 nullptr）。
    // 非空时连接成功后先在 Reactor 线程驱动握手，handshake->done() 后置空并进入 splice。
    // 仅 Reactor 线程访问。
    std::unique_ptr<TcpHandshake> handshake;
    // 握手待发字节中尚未写完的残留（非阻塞 write 部分写时保留续发）。仅 Reactor 线程访问。
    std::string handshakeOutbox;

    IP4 origDst;
    uint16_t origDstPort;
    IP4 origSrc;

    std::mutex bufMutex;
    std::string forwardBuf;
    bool wantWrite;

    // lwIP -> fd 方向背压核心：已写进落地 fd、待向 lwIP 确认（tcp_recved）的字节数。
    // 由 Reactor 线程累加、NetStack 线程在 ackRecvedToLwip 中消费，跨线程用原子传递。
    // 延迟确认使 lwIP 接收窗口只在数据真正落地后才推进，从而把源端发送速率自动约束在
    // 落地 fd 的消费能力之内，forwardBuf 天然被限制在一个 TCP_WND 内，杜绝无界膨胀。
    std::atomic<size_t> pendingRecved{0};

    // fd -> lwIP 方向待发缓冲（仅 NetStack 线程访问）。
    // tcp_sndbuf 满时暂存，onSent 释放空间后继续 pump，避免静默丢包。
    std::string backwardBuf;
    std::atomic<bool> readPaused{false};
    bool fdEof;
    // fd -> lwIP 方向"已从落地 fd 读出但尚未被 lwIP 确认排空"的字节积压。
    // 由 Reactor 线程（读出后 +）与 NetStack 线程（pump 写入 lwIP 后 -）共同维护，
    // 用于在 Reactor 读取侧做同步背压：超过高水位立即停读，回落到低水位再恢复，
    // 避免单线程 NetStack 被海量 writeToLwip 任务淹没、导致回程 ACK 饿死、发送窗口卡死。
    std::atomic<size_t> backwardBytes{0};

    std::shared_ptr<SessionTcp> self;
};

} // namespace candy

#endif
