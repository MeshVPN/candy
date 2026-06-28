// SPDX-License-Identifier: MIT
#ifndef CANDY_CLI_CONFIG_H
#define CANDY_CLI_CONFIG_H

#include <Poco/JSON/Object.h>
#include <map>
#include <string>

struct arguments {
    int parse(int argc, char *argv[]);
    Poco::JSON::Object json();

private:
    void parseFile(std::string cfgFile);
    std::map<std::string, std::string> fileToKvMap(const std::string &filename);

    std::string mode;
    std::string websocket;
    std::string password;
    bool noTimestamp = false;
    bool debug = false;

    std::string dhcp;
    std::string sdwan;

    std::string name;
    std::string tun;
    std::string stun;
    std::string localhost;
    int port = 0;
    int discovery = 0;
    int routeCost = 0;
    int mtu = 1400;
    std::string forwardMode = "kernel";
    // 阶段三：外部 socks5 上游与分流规则（仅 userspace 模式有效，空表示全部 direct）。
    std::string socks5Upstream;
    std::string outboundRules;
};

int saveTunAddress(const std::string &name, const std::string &cidr);
std::string loadTunAddress(const std::string &name);
std::string virtualMac(const std::string &name);
std::string storageDirectory(std::string subdir = "");

#endif
