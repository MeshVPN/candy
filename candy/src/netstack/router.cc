// SPDX-License-Identifier: MIT
#include "netstack/router.h"

namespace candy {

Router::Router() : defaultOutbound("direct") {}

void Router::addRule(const Rule &rule) {
    this->rules.push_back(rule);
}

void Router::setDefault(const std::string &outbound) {
    this->defaultOutbound = outbound;
}

std::string Router::match(const FlowKey &flow) const {
    for (const auto &rule : this->rules) {
        switch (rule.type) {
        case MatchType::DstCidr:
            // dst & mask == cidr & mask 即视为命中网段。
            if ((flow.dst & rule.mask) == (rule.cidr & rule.mask)) {
                return rule.outbound;
            }
            break;
        case MatchType::DstPort:
            if (flow.dstPort == rule.port) {
                return rule.outbound;
            }
            break;
        case MatchType::Default:
            return rule.outbound;
        }
    }
    return this->defaultOutbound;
}

} // namespace candy
