// SPDX-License-Identifier: MIT
#include "netstack/socks5_client.h"
#include <cstring>

namespace candy {

void Socks5Client::startConnectIPv4(IP4 dstHost, uint16_t dstPort, const std::string &username,
                                    const std::string &password) {
    this->command = Socks5::CMD_CONNECT;
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
    this->command = Socks5::CMD_CONNECT;
    this->useDomain = true;
    this->dstDomain = domain;
    this->dstPort = dstPort;
    this->username = username;
    this->password = password;
    this->useUserPass = !username.empty();
    buildAuthMethods();
    this->st = State::WriteAuthMethods;
}

void Socks5Client::startUdpAssociate(const std::string &username, const std::string &password) {
    // UDP ASSOCIATE：CMD=0x03，请求里的 DST.ADDR/DST.PORT 填 0.0.0.0:0（RFC 1928 §7：
    // 客户端尚不确定将从哪个源端口发 UDP，置 0 让服务端不做限制）。其余握手流程与 CONNECT 相同。
    this->command = Socks5::CMD_UDP_ASSOCIATE;
    this->useDomain = false;
    this->dstHost = IP4("0.0.0.0");
    this->dstPort = 0;
    this->username = username;
    this->password = password;
    this->useUserPass = !username.empty();
    buildAuthMethods();
    this->st = State::WriteAuthMethods;
}

IP4 Socks5Client::udpRelayHost() const {
    return this->relayAddr;
}

uint16_t Socks5Client::udpRelayPort() const {
    return this->relayPortValue;
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
    // VER(1) CMD(1) RSV(1=0) ATYP(1) DST.ADDR DST.PORT。CMD 取 this->command
    // （CONNECT 或 UDP ASSOCIATE）。
    this->outbound.clear();
    this->outbound.push_back(Socks5::VERSION);
    this->outbound.push_back(this->command);
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
            // UDP ASSOCIATE：BND.ADDR/BND.PORT 即服务端分配的 UDP 中继端点，需解析留用。
            // 仅支持 IPv4 中继地址（与本工程 IPv4-only 一致）。若服务端返回 0.0.0.0，
            // 按 RFC 1928 约定回退到控制连接的对端地址（由调用方处理，这里置 0）。
            if (this->command == Socks5::CMD_UDP_ASSOCIATE) {
                if (atyp != Socks5::ATYP_IPV4) {
                    fail("udp associate reply with non-IPv4 relay addr");
                    return false;
                }
                uint32_t v = 0;
                std::memcpy(&v, &this->inbound[4], 4); // 网络序字节直存即 a.b.c.d
                std::memcpy(&this->relayAddr, &v, 4);  // IP4 为 packed 4 字节，按内存序直存
                uint16_t portBE = 0;
                std::memcpy(&portBE, &this->inbound[4 + addrLen], 2);
                this->relayPortValue = ntoh<uint16_t>(portBE);
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

std::string socks5UdpEncap(IP4 dstHost, uint16_t dstPort, const uint8_t *data, size_t len) {
    // RSV(2=0) FRAG(1=0) ATYP(1=IPv4) DST.ADDR(4) DST.PORT(2) DATA。
    std::string out;
    out.reserve(10 + len);
    out.push_back(0x00); // RSV
    out.push_back(0x00); // RSV
    out.push_back(0x00); // FRAG
    out.push_back((char)Socks5::ATYP_IPV4);
    uint32_t v = uint32_t(dstHost); // 内存即网络序 a.b.c.d
    out.append(reinterpret_cast<const char *>(&v), 4);
    uint16_t portBE = hton<uint16_t>(dstPort);
    out.append(reinterpret_cast<const char *>(&portBE), 2);
    if (data != nullptr && len > 0) {
        out.append(reinterpret_cast<const char *>(data), len);
    }
    return out;
}

bool socks5UdpDecap(const uint8_t *pkt, size_t len, IP4 &srcHost, uint16_t &srcPort, const uint8_t **payload,
                    size_t &payloadLen) {
    // 最小头长：RSV(2) FRAG(1) ATYP(1) = 4 字节，之后按 ATYP 取地址。
    if (pkt == nullptr || len < 4) {
        return false;
    }
    uint8_t frag = pkt[2];
    if (frag != 0x00) {
        return false; // 不支持分片，丢弃
    }
    uint8_t atyp = pkt[3];
    size_t addrLen = 0;
    switch (atyp) {
    case Socks5::ATYP_IPV4:
        addrLen = 4;
        break;
    case Socks5::ATYP_IPV6:
        addrLen = 16;
        break;
    case Socks5::ATYP_DOMAIN:
        if (len < 5) {
            return false;
        }
        addrLen = 1 + pkt[4];
        break;
    default:
        return false;
    }
    size_t headLen = 4 + addrLen + 2;
    if (len < headLen) {
        return false;
    }
    // 仅 IPv4 源地址可还原为 IP4；其余类型也接受但 srcHost 置 0（回包寻址不依赖它）。
    if (atyp == Socks5::ATYP_IPV4) {
        uint32_t v = 0;
        std::memcpy(&v, &pkt[4], 4);
        std::memcpy(&srcHost, &v, 4);
    } else {
        srcHost = IP4("0.0.0.0");
    }
    uint16_t portBE = 0;
    std::memcpy(&portBE, &pkt[4 + addrLen], 2);
    srcPort = ntoh<uint16_t>(portBE);
    *payload = pkt + headLen;
    payloadLen = len - headLen;
    return true;
}

} // namespace candy
