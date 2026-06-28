// SPDX-License-Identifier: MIT
#ifndef CANDY_NETSTACK_ROUTER_H
#define CANDY_NETSTACK_ROUTER_H

#include "core/net.h"
#include <cstdint>
#include <string>
#include <vector>

namespace candy {

// Router：规则引擎（阶段三）。在 tun 入口、lwIP 终结之前，按目的网段/端口/兜底规则
// 为一条流选择 outbound（mesh / direct / external-socks5）。
//
// 匹配只依赖 L3 包头即可完成的字段（origSrc/origDst/proto/dstPort），与 03 文档 §5.2
// 一致：分流发生在终结之前，只有命中需要本机终结的出口才进入 lwIP。
//
// 当前为阶段三骨架：规则容器与 match 已就绪，但尚未接入 tun.cc 热路径，default 兜底
// 返回 "direct"，因此对现有 kernel/userspace 行为零影响。后续接入时由配置注入规则。
class Router {
public:
    // 匹配输入：从原始 IP 包头解析得到的 L3 信息（无需终结）。
    struct FlowKey {
        IP4 src;
        IP4 dst;
        uint8_t proto;
        uint16_t dstPort;
    };

    // 规则条件类型（按 03 文档 §5.1，自上而下优先匹配）。
    enum class MatchType {
        DstCidr,  // 目的网段：dst ∈ cidr
        DstPort,  // 目的端口
        Default,  // 兜底
    };

    struct Rule {
        MatchType type;
        // DstCidr 时生效：网段与掩码。
        IP4 cidr;
        IP4 mask;
        // DstPort 时生效。
        uint16_t port;
        // 命中后选用的 outbound 名称（direct / mesh / socks5 / 组名）。
        std::string outbound;
    };

    Router();

    // 追加一条规则（按追加顺序即为匹配优先级）。
    void addRule(const Rule &rule);

    // 设置兜底 outbound（无显式 Default 规则时使用）。
    void setDefault(const std::string &outbound);

    // 为一条流选择 outbound 名称；无任何规则命中时返回兜底（默认 "direct"）。
    std::string match(const FlowKey &flow) const;

    bool empty() const { return this->rules.empty(); }

private:
    std::vector<Rule> rules;
    std::string defaultOutbound;
};

} // namespace candy

#endif
