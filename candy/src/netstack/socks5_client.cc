// SPDX-License-Identifier: MIT
#include "netstack/socks5_client.h"
#include <cstring>

namespace candy {

void Socks5Client::startConnectIPv4(IP4 dstHost, uint16_t dstPort, const std::string &username,
                                    const std::string &password) {
    this->useDomain = false;
    this->dstHost = dstHost;
    this->dstPort = dstPort;
    this->username = username;
    this->password = password;
    this->useUserPass = !username.empty();
    buildAuthMethods();
    this->st = State::WriteAuthMethods;
}

void Socks5Client::startConnectDomain(const std::string &domain, uint16_t dstPort, const std::string &username,
                                      const std::string &password) {
    this->useDomain = true;
    this->dstDomain = domain;
    this->dstPort = dstPort;
    this->username = username;
    this->password = password;
    this->useUserPass = !username.empty();
    buildAuthMethods();
    this->st = State::WriteAuthMethods;
}

void Socks5Client::buildAuthMethods() {
    // VER(1) NMETHODS(1) METHODS(nmethods)。无认证只声明 NONE；
    // 配了用户口令则同时声明 NONE 与 USERPASS，让服务端择优。
    this->outbound.clear();
    this->outbound.push_back(Socks5::VERSION);
    if (this->useUserPass) {
        this->outbound.push_back(0x02);
        this->outbound.push_back(Socks5::METHOD_NONE);
        this->outbound.push_back(Socks5::METHOD_USERPASS);
    } else {
        this->outbound.push_back(0x01);
        this->outbound.push_back(Socks5::METHOD_NONE);
    }
}

void Socks5Client::buildAuthCreds() {
    // RFC 1929：VER(1=0x01) ULEN(1) UNAME(ulen) PLEN(1) PASSWD(plen)。
    this->outbound.clear();
    this->outbound.push_back(Socks5::AUTH_VERSION);
    this->outbound.push_back((uint8_t)this->username.size());
    this->outbound.insert(this->outbound.end(), this->username.begin(), this->username.end());
    this->outbound.push_back((uint8_t)this->password.size());
    this->outbound.insert(this->outbound.end(), this->password.begin(), this->password.end());
}

void Socks5Client::buildRequest() {
    // VER(1) CMD(1) RSV(1=0) ATYP(1) DST.ADDR DST.PORT。
    this->outbound.clear();
    this->outbound.push_back(Socks5::VERSION);
    this->outbound.push_back(Socks5::CMD_CONNECT);
    this->outbound.push_back(0x00);
    if (this->useDomain) {
        this->outbound.push_back(Socks5::ATYP_DOMAIN);
        this->outbound.push_back((uint8_t)this->dstDomain.size());
        this->outbound.insert(this->outbound.end(), this->dstDomain.begin(), this->dstDomain.end());
    } else {
        this->outbound.push_back(Socks5::ATYP_IPV4);
        // IP4 的内存字节即网络序的 a.b.c.d，直接拷 4 字节即为 socks5 所需顺序。
        uint32_t v = uint32_t(this->dstHost);
        const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
        this->outbound.insert(this->outbound.end(), p, p + 4);
    }
    // 端口为大端（网络序）。
    uint16_t portBE = hton<uint16_t>(this->dstPort);
    const uint8_t *pp = reinterpret_cast<const uint8_t *>(&portBE);
    this->outbound.insert(this->outbound.end(), pp, pp + 2);
}

bool Socks5Client::feed(const uint8_t *data, size_t len) {
    if (this->st == State::Failed) {
        return false;
    }
    if (this->st == State::Done) {
        // 握手已完成，多余字节归入 leftover。
        this->leftover.insert(this->leftover.end(), data, data + len);
        return true;
    }
    this->inbound.insert(this->inbound.end(), data, data + len);

    // 循环推进：可能一次 feed 跨越多个状态（例如认证应答与请求应答粘包）。
    for (;;) {
        switch (this->st) {
        case State::ReadAuthMethod: {
            // 应答固定 2 字节：VER, METHOD。
            if (this->inbound.size() < 2) {
                return true;
            }
            uint8_t ver = this->inbound[0];
            uint8_t method = this->inbound[1];
            this->inbound.erase(this->inbound.begin(), this->inbound.begin() + 2);
            if (ver != Socks5::VERSION) {
                fail("bad socks version in auth method reply");
                return false;
            }
            if (method == Socks5::METHOD_NONE) {
                buildRequest();
                this->st = State::WriteRequest;
            } else if (method == Socks5::METHOD_USERPASS) {
                if (this->username.empty()) {
                    fail("server requires userpass but no credentials configured");
                    return false;
                }
                buildAuthCreds();
                this->st = State::WriteAuthCreds;
            } else {
                fail("server rejected all auth methods");
                return false;
            }
            // 进入 Write* 状态后没有更多可解析的入站数据，交回调用方发送。
            return true;
        }
        case State::ReadAuthCreds: {
            // RFC 1929 应答固定 2 字节：VER, STATUS(0=成功)。
            if (this->inbound.size() < 2) {
                return true;
            }
            uint8_t status = this->inbound[1];
            this->inbound.erase(this->inbound.begin(), this->inbound.begin() + 2);
            if (status != 0x00) {
                fail("socks5 userpass auth failed");
                return false;
            }
            buildRequest();
            this->st = State::WriteRequest;
            return true;
        }
        case State::ReadResponse: {
            // VER(1) REP(1) RSV(1) ATYP(1) BND.ADDR BND.PORT。先读前 4 字节定长头。
            if (this->inbound.size() < 4) {
                return true;
            }
            uint8_t ver = this->inbound[0];
            uint8_t rep = this->inbound[1];
            uint8_t atyp = this->inbound[3];
            if (ver != Socks5::VERSION) {
                fail("bad socks version in connect reply");
                return false;
            }
            if (rep != Socks5::REP_SUCCESS) {
                fail("socks5 connect rejected, rep=" + std::to_string(rep));
                return false;
            }
            // 计算绑定地址长度，确认应答完整收齐。
            size_t addrLen = 0;
            switch (atyp) {
            case Socks5::ATYP_IPV4:
                addrLen = 4;
                break;
            case Socks5::ATYP_IPV6:
                addrLen = 16;
                break;
            case Socks5::ATYP_DOMAIN:
                if (this->inbound.size() < 5) {
                    return true; // 还没收到域名长度字节
                }
                addrLen = 1 + this->inbound[4];
                break;
            default:
                fail("unknown atyp in connect reply");
                return false;
            }
            size_t total = 4 + addrLen + 2; // 头 + 地址 + 端口
            if (this->inbound.size() < total) {
                return true; // 等待剩余字节
            }
            // 应答收齐，多余的即为业务数据。
            this->leftover.assign(this->inbound.begin() + total, this->inbound.end());
            this->inbound.clear();
            this->st = State::Done;
            return true;
        }
        default:
            // Write* 状态：等待调用方把 outbound 发出去后再调用 advanceAfterWrite()。
            return true;
        }
    }
}

std::vector<uint8_t> Socks5Client::takeOutbound() {
    std::vector<uint8_t> out;
    out.swap(this->outbound);
    // 发送完成意味着对应的 Write 状态结束，迁移到等待应答的 Read 状态。
    switch (this->st) {
    case State::WriteAuthMethods:
        this->st = State::ReadAuthMethod;
        break;
    case State::WriteAuthCreds:
        this->st = State::ReadAuthCreds;
        break;
    case State::WriteRequest:
        this->st = State::ReadResponse;
        break;
    default:
        break;
    }
    return out;
}

bool Socks5Client::hasOutbound() const {
    return !this->outbound.empty();
}

std::vector<uint8_t> Socks5Client::takeLeftover() {
    std::vector<uint8_t> out;
    out.swap(this->leftover);
    return out;
}

Socks5Client::State Socks5Client::state() const {
    return this->st;
}

bool Socks5Client::done() const {
    return this->st == State::Done;
}

bool Socks5Client::failed() const {
    return this->st == State::Failed;
}

const std::string &Socks5Client::error() const {
    return this->errMsg;
}

void Socks5Client::fail(const std::string &reason) {
    this->errMsg = reason;
    this->st = State::Failed;
}

} // namespace candy
