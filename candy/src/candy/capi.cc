// SPDX-License-Identifier: MIT
#include "candy/capi.h"
#include "candy/client.h"
#include "candy/common.h"
#include "utils/log.h"
#include <Poco/Format.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace {

// C ABI 层持有 candy_start 起的工作线程；candy_stop 时 join，保证干净退出。
// candy::client 内部已管理实例生命周期，这里只负责线程归属与 join。
std::map<std::string, std::thread> g_threads;
std::mutex g_threadsMutex;

// 复制 std::string 到 malloc 内存，交由调用方 candy_string_free 释放。
char *dupString(const std::string &s) {
    char *p = static_cast<char *>(malloc(s.size() + 1));
    if (p) {
        memcpy(p, s.c_str(), s.size() + 1);
    }
    return p;
}

} // namespace

extern "C" {

int candy_start(const char *id, const char *json) {
    if (!id || !json) {
        candy::logger().error("[capi] candy_start invalid arg: null id or json");
        return CANDY_ERR_INVALID_ARG;
    }
    std::string sid(id), sjson(json);
    candy::logger().information(Poco::format("[capi] candy_start enter: id=%s", sid));

    // JSON 解析在 ABI 边界内完成，任何异常都不外泄，统一转错误码。
    Poco::JSON::Object::Ptr cfg;
    try {
        Poco::JSON::Parser parser;
        cfg = parser.parse(sjson).extract<Poco::JSON::Object::Ptr>();
    } catch (const std::exception &e) {
        candy::logger().error(Poco::format("[capi] candy_start bad config: id=%s err=%s", sid, std::string(e.what())));
        return CANDY_ERR_BAD_CONFIG;
    } catch (...) {
        candy::logger().error(Poco::format("[capi] candy_start bad config: id=%s err=unknown", sid));
        return CANDY_ERR_BAD_CONFIG;
    }
    if (cfg.isNull()) {
        candy::logger().error(Poco::format("[capi] candy_start bad config: id=%s err=not a json object", sid));
        return CANDY_ERR_BAD_CONFIG;
    }

    std::lock_guard<std::mutex> lock(g_threadsMutex);
    if (g_threads.count(sid)) {
        candy::logger().warning(Poco::format("[capi] candy_start already running: id=%s", sid));
        return CANDY_ERR_ALREADY_RUNNING;
    }

    // candy::client::run 是阻塞的（内部断线重连循环），放到独立线程运行。
    g_threads.emplace(sid, std::thread([sid, cfg]() {
                          candy::logger().information(Poco::format("[capi] worker started: id=%s", sid));
                          try {
                              candy::client::run(sid, *cfg);
                          } catch (const std::exception &e) {
                              candy::logger().error(
                                  Poco::format("[capi] worker exception: id=%s err=%s", sid, std::string(e.what())));
                          } catch (...) {
                              candy::logger().error(Poco::format("[capi] worker exception: id=%s err=unknown", sid));
                          }
                          candy::logger().information(Poco::format("[capi] worker exited: id=%s", sid));
                      }));
    candy::logger().information(Poco::format("[capi] candy_start ok: id=%s", sid));
    return CANDY_OK;
}

int candy_stop(const char *id) {
    if (!id) {
        candy::logger().error("[capi] candy_stop invalid arg: null id");
        return CANDY_ERR_INVALID_ARG;
    }
    std::string sid(id);
    candy::logger().information(Poco::format("[capi] candy_stop enter: id=%s", sid));

    std::thread th;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        auto it = g_threads.find(sid);
        if (it == g_threads.end()) {
            candy::logger().warning(Poco::format("[capi] candy_stop not found: id=%s", sid));
            return CANDY_ERR_NOT_FOUND;
        }
        th = std::move(it->second);
        g_threads.erase(it);
    }

    // 置停止标志：各线程 1s 轮询内感知并退出重连循环。
    candy::client::shutdown(sid);
    if (th.joinable()) {
        th.join();
    }
    candy::logger().information(Poco::format("[capi] candy_stop ok: id=%s", sid));
    return CANDY_OK;
}

char *candy_status(const char *id) {
    if (!id) {
        candy::logger().error("[capi] candy_status invalid arg: null id");
        return nullptr;
    }
    try {
        auto st = candy::client::status(id);
        if (!st) {
            return nullptr;
        }
        std::ostringstream oss;
        st->stringify(oss);
        return dupString(oss.str());
    } catch (const std::exception &e) {
        candy::logger().error(
            Poco::format("[capi] candy_status exception: id=%s err=%s", std::string(id), std::string(e.what())));
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

void candy_string_free(char *s) {
    free(s);
}

const char *candy_version(void) {
    // 静态存活于进程生命周期，返回裸指针不需调用方释放。
    static std::string v = candy::version();
    return v.c_str();
}

char *candy_create_vmac(void) {
    try {
        return dupString(candy::create_vmac());
    } catch (...) {
        return nullptr;
    }
}

int candy_set_tun_fd(const char *id, int fd) {
    // 第二阶段：接到 MobileTun::fromFd。当前仅记录调用，返回未实现。
    candy::logger().information(
        Poco::format("[capi] candy_set_tun_fd called (unimplemented): id=%s fd=%d", std::string(id ? id : ""), fd));
    return CANDY_ERR_UNIMPLEMENTED;
}

} // extern "C"
