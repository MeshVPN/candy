// SPDX-License-Identifier: MIT
#ifndef CANDY_NETSTACK_STUN_H
#define CANDY_NETSTACK_STUN_H

// 标准 STUN(RFC5389) 识别与分流工具（仅供 UDP 单端口收敛的 demux 使用）。
//
// 背景（见方案 §11.2/§11.4）：单端口收敛后，多个内部源共用一个出口 fd，回程 demux 依赖
// 「该包是否带有 E 可解析且对每源唯一的标识」。STUN 的 96-bit transaction id 正是这种标识：
//   - 出站 Binding Request 打洞并建 srflx 候选，本机按 txid 登记归属；
//   - 回程 Binding Response 靠 txid 精确还原到源（即便多源共享同一公共 STUN 服务器）；
//   - 故 STUN 一律豁免数据面 FCFS（否则第二源发往共享 STUN 会被误丢，导致 gather 失败）。
//
// 支持范围：**仅标准 STUN(RFC5389，带 magic cookie 0x2112A442)**；RFC3489 老式 STUN 无
// magic cookie，不在支持范围（不识别，按普通数据面处理）。

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace candy {

// STUN 消息 class（RFC5389 §6：method 与 class 交织编码在 14-bit message type 内，
// class 由 bit8(C1)、bit4(C0) 两位构成，与具体 method 无关）。按 class 分流即可，
// 无需枚举各 method（Binding/Allocate/… 处理方式一致）。
enum class StunClass : uint8_t {
    Request = 0,      // 0b00：请求（出站含 txid 需登记；入站为对端 ICE 连通性检查）
    Indication = 1,   // 0b01：指示（如 keepalive Binding Indication 0x0011，无需回程配对）
    SuccessResp = 2,  // 0b10：成功响应（回程靠 txid 还原）
    ErrorResp = 3,    // 0b11：错误响应（回程靠 txid 还原）
};

struct StunInfo {
    StunClass klass;   // 消息 class
    uint8_t txid[12];  // 96-bit transaction id
};

// STUN magic cookie（RFC5389，位于 byte4-7，网络序）。
inline constexpr uint32_t kStunMagicCookie = 0x2112A442u;

// 识别一段载荷是否为标准 STUN 消息。命中则回填 out 并返回 true。
//
// 三重校验（见 §11.4「STUN 识别」，避免把恰好前 4 字节等于 magic cookie 的数据包误判）：
//   ① byte4-7 == 0x2112A442（magic cookie）
//   ② 消息类型最高 2 bit == 0（byte0 高 2 位为 0，STUN 头部固定前导）
//   ③ length 字段(byte2-3) == 总长-20 且 4 字节对齐
inline bool stunParse(const void *data, size_t len, StunInfo *out) {
    // STUN 头部固定 20 字节：2(type)+2(length)+4(cookie)+12(txid)。
    if (data == nullptr || len < 20) {
        return false;
    }
    const uint8_t *b = static_cast<const uint8_t *>(data);

    // ② 消息类型最高 2 bit 必须为 0。
    if ((b[0] & 0xC0) != 0) {
        return false;
    }

    // ① magic cookie（byte4-7，网络序）。
    uint32_t cookie = (uint32_t(b[4]) << 24) | (uint32_t(b[5]) << 16) | (uint32_t(b[6]) << 8) | uint32_t(b[7]);
    if (cookie != kStunMagicCookie) {
        return false;
    }

    // ③ length 字段(byte2-3，网络序) 必须等于「总长-20」且 4 字节对齐。
    uint16_t msgLen = (uint16_t(b[2]) << 8) | uint16_t(b[3]);
    if ((msgLen & 0x3) != 0) {
        return false;
    }
    if (size_t(msgLen) + 20 != len) {
        return false;
    }

    if (out != nullptr) {
        // class 由 message type 的 bit8(C1)、bit4(C0) 两位构成。
        uint16_t type = (uint16_t(b[0]) << 8) | uint16_t(b[1]);
        uint8_t c1 = (type >> 8) & 0x1;
        uint8_t c0 = (type >> 4) & 0x1;
        out->klass = static_cast<StunClass>((c1 << 1) | c0);
        std::memcpy(out->txid, b + 8, 12);
    }
    return true;
}

} // namespace candy

#endif
