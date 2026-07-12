# Candy Flutter 客户端骨架（macOS 桌面）

dart:ffi 集成 candy C ABI 核心库（`candy/include/candy/capi.h`）的最小客户端：连接 / 断开 / 状态查询闭环。

## 目录

```
clients/flutter/
├── pubspec.yaml
├── lib/
│   ├── candy_bindings.dart   # dart:ffi 裸绑定，一一对应 capi.h
│   ├── candy_service.dart    # 业务封装：字符串编解码 / 内存释放 / JSON
│   └── main.dart             # 连接/断开/状态查询 UI
└── tool/
    ├── ffi_smoke.dart        # 只读接口冒烟（version/vmac/status）
    └── e2e_connect.dart      # 端到端可用性：真连服务端拿下发地址
```

> 本目录只含**可控核心**（FFI + UI）。`macos/` 等平台样板目录未手写，需用 `flutter create` 补全（见下）。

## 一、编译 native 动态库（在本仓库根 `candy/`）

三个桌面平台共用同一开关 `-DCANDY_SHARED=ON`，Dart 侧按平台名自动加载，代码零改动复用：

| 平台    | 构建命令（仓库根执行）                                                                                                            | 产物                            |
| ------- | ------------------------------------------------------------------------------------------------------------------------------- | ------------------------------- |
| macOS   | `cmake -S . -B build-mac -DCANDY_SHARED=ON -DCANDY_NETSTACK=ON && cmake --build build-mac --target candy_shared -j`             | `build-mac/libcandy_ffi.dylib`  |
| Linux   | `cmake -S . -B build-linux -DCANDY_SHARED=ON -DCANDY_NETSTACK=ON && cmake --build build-linux --target candy_shared -j`         | `build-linux/libcandy_ffi.so`   |
| Windows | `cmake -S . -B build-win -DCANDY_SHARED=ON -DCANDY_NETSTACK=ON && cmake --build build-win --target candy_shared --config Release` | `build-win/Release/candy_ffi.dll` |

三端产物均导出同一组 `candy_*` 符号（`extern "C"`，无名字修饰），`candy_bindings.dart` 的 `_libraryPath()` 已按
`libcandy_ffi.dylib` / `libcandy_ffi.so` / `candy_ffi.dll` 命名匹配。

`CANDY_NETSTACK=ON` 提供 userspace lwIP 栈；桌面 userspace 模式必需。

> **Linux（尤其 aarch64）**：链入动态库的静态库须为位置无关代码，顶层 CMake 已对 `candy-library` 与 `lwip` 开启
> `POSITION_INDEPENDENT_CODE`（macOS 默认容错、Windows 该属性无意义，均无副作用）。
> **Windows**：`candy-library` 的 `ws2_32`/`iphlpapi` 及 Poco/OpenSSL 虽是 PRIVATE 依赖，但对静态库会以 LINK_ONLY
> 传递给 `candy_shared`，无需额外配置；wintun 为运行时 `LoadLibraryExW` 加载，非链接期依赖，不影响 .dll 产出。

## 二、补全 Flutter macOS 平台样板

本机若未装 Flutter，先安装 Flutter SDK，然后在本目录执行：

```bash
cd clients/flutter
flutter create --platforms=macos --project-name candy .
flutter pub get
```

`flutter create` 只会补 `macos/` 等缺失目录，不会覆盖已存在的 `lib/` 与 `pubspec.yaml`。

## 三、让 App 找到动态库

`CandyBindings.open()` 用 `DynamicLibrary.open('libcandy_ffi.dylib')` 按名加载，需让 dylib 位于 App 可搜索路径。开发期最简单：把 dylib 拷到构建产物同目录，或在 Xcode 的 Runner target 加 “Copy Files” 阶段把 `libcandy_ffi.dylib` 放进 `Contents/Frameworks/` 并设 rpath。

开发期快速验证也可临时改 `candy_bindings.dart` 里 `_libraryPath()` 返回 dylib 绝对路径。

## 四、运行

```bash
flutter run -d macos
```

填 WebSocket 地址与密码，点“连接”，状态卡每 2s 轮询 `candy_status` 显示分配到的地址。

## 五、无 GUI 验证可用性（不依赖 Xcode/CocoaPods）

本机若缺完整 Xcode，无法 `flutter build/run macos`，可用纯 Dart 脚本验证「Dart 复用 native 库 + 真实连接」这条核心链路：

```bash
# 1) 只读接口冒烟
dart run tool/ffi_smoke.dart /abs/build-mac/libcandy_ffi.dylib

# 2) 端到端连接（另开终端起一个本地 DHCP 服务端）
./build-mac/candy-cli/candy -m server -w ws://127.0.0.1:18899 -p e2e-secret -d 10.99.0.0/24
dart run tool/e2e_connect.dart /abs/build-mac/libcandy_ffi.dylib ws://127.0.0.1:18899 e2e-secret 10.99.0.
```

`e2e_connect.dart` 走的是 GUI 同一套 `CandyService` API：作为 DHCP 客户端连接，`candy_status` 拿到服务端从地址池下发的地址即证明 WebSocket 鉴权往返成功（地址在建 tun 设备前就已拿到，**全程无需 root**）。期望输出 `E2E_OK`。

## 六、跨平台发布（GitHub Actions）

`.github/workflows/flutter.yaml`：一套 Dart 代码 × 三个原生 runner。每个 runner 各自 ① CMake 编 `candy_ffi` 动态库 ② `flutter create` 补平台样板 ③ `flutter build` 出产物 ④ 把动态库塞进 App 可加载路径（mac `Contents/Frameworks/`、Linux `bundle/lib/`、Windows exe 同目录）⑤ 打包上传。`push master` 冒烟构建，`release published` 时把 zip/tar.gz 挂到 Release。`_libraryPath()` 已相对可执行文件定位，与打包路径一致。

## 说明与后续

- macOS userspace tun 需相应权限；若用系统 utun 自建模型，App 需相应授权。
- 移动端（Android/iOS）需实现 `MobileTun`（从系统 fd 构造）并接通 `candy_set_tun_fd`，当前该接口返回 `CANDY_ERR_UNIMPLEMENTED`。
- Windows/Linux 桌面：绑定层已按 `candy_ffi.dll` / `libcandy_ffi.so` 命名预留，编出对应动态库即可复用同一套 Dart 代码。
