// SPDX-License-Identifier: MIT
#ifndef CANDY_CAPI_H
#define CANDY_CAPI_H

// 纯 C ABI 导出层：供 Windows / macOS / Android / iOS 四端 FFI 消费。
// 本头文件刻意不引入任何 Poco / C++ 类型，保证 JNI / Swift / dart:ffi 直接可用。
//
// 动态库使用约定：
//   - 构建动态库时：构建方定义 CANDY_BUILD_DLL（导出符号）；
//   - 使用动态库时：使用方定义 CANDY_USE_DLL（导入符号）；
//   - 静态链接时：两者都不定义，CANDY_API 展开为空。

#ifdef __cplusplus
extern "C" {
#endif

// 导出宏：Windows 用 __declspec，其余平台用 visibility("default")。
#if defined(_WIN32)
#if defined(CANDY_BUILD_DLL)
#define CANDY_API __declspec(dllexport)
#elif defined(CANDY_USE_DLL)
#define CANDY_API __declspec(dllimport)
#else
#define CANDY_API
#endif
#else
#define CANDY_API __attribute__((visibility("default")))
#endif

// 错误码：函数返回 int（ABI 稳定），取值见此枚举。
typedef enum {
    CANDY_OK = 0,                   // 成功
    CANDY_ERR_INVALID_ARG = -1,     // 空指针等非法入参
    CANDY_ERR_ALREADY_RUNNING = -2, // 同 id 实例已存在
    CANDY_ERR_NOT_FOUND = -3,       // 无此 id 实例
    CANDY_ERR_BAD_CONFIG = -4,      // JSON 解析失败
    CANDY_ERR_UNIMPLEMENTED = -5,   // 功能尚未接通（如 set_tun_fd）
    CANDY_ERR_INTERNAL = -6,        // 其他内部异常
} candy_result;

// 启动一个客户端实例（非阻塞：内部起线程运行阻塞式重连循环）。
// 返回 CANDY_OK 仅表示线程已启动，不代表已连接成功；配置字段缺失等运行期错误
// 会在运行线程内以 "[capi] worker exception" 日志报出。
//   id   实例标识（移动端单实例可传 "default"）
//   json 配置 JSON，必填 name/password/websocket/tun/vmac/expt/stun/
//        discovery/route/mtu/port/localhost；可选 userspace-stack(bool)
CANDY_API int candy_start(const char *id, const char *json);

// 停止实例（同步：置停止标志后 join 工作线程，通常 1s 内返回）。
CANDY_API int candy_stop(const char *id);

// 查询状态：返回 malloc 分配的 JSON 字符串（如 {"address":"..."}），
// 调用方须用 candy_string_free 释放；无此实例返回 NULL。
CANDY_API char *candy_status(const char *id);

// 释放本库返回的堆字符串。
CANDY_API void candy_string_free(char *s);

// 版本号：返回进程内静态字符串，切勿释放。
CANDY_API const char *candy_version(void);

// 生成虚拟 MAC：返回 malloc 字符串，须用 candy_string_free 释放。
CANDY_API char *candy_create_vmac(void);

// —— 移动端第二阶段接口（MobileTun 落地后接通）——
// 注入系统分配的 tun fd（Android VpnService / iOS NEPacketTunnelProvider）。
// 当前返回 CANDY_ERR_UNIMPLEMENTED：契约先行，四端桥接代码无需返工。
CANDY_API int candy_set_tun_fd(const char *id, int fd);

#ifdef __cplusplus
}
#endif

#endif
