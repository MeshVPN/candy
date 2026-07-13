# Candy Flutter 客户端（macOS 桌面）

Flutter GUI + 特权 daemon 架构的 candy 桌面客户端：连接 / 断开 / 状态查询闭环。

## 架构

```
Flutter GUI(普通用户) ──HTTP(localhost:26817)──► candy-service(root daemon) ──► candy 核心 ──► 建 utun
```

建 utun 是特权操作，普通用户进程无权直接建网卡。方案参照 ClashX / Tunnelblick：
把项目里现成的 `candy-service`（Poco HTTP 守护进程）装成 root 的 **LaunchDaemon** 常驻，
GUI 以普通用户身份通过 localhost HTTP 驱动它。首次连接时用 `osascript` 触发一次系统
授权把 daemon 装好，之后开机自启、永久免密。

> 全程 **不改核心 C++**：GUI 只是 HTTP 调用方，`candy-service` 与 candy 核心均为现成代码。

## 目录

```
clients/flutter/
├── pubspec.yaml
├── lib/
│   ├── candy_daemon_client.dart  # daemon HTTP 通信层（/api/run /api/status /api/shutdown）
│   ├── candy_service.dart        # 业务封装：config 补全 / vmac 生成 / 状态解析
│   ├── daemon_installer.dart     # 纯 Dart 提权安装器（osascript + LaunchDaemon）
│   └── main.dart                 # 连接/断开/状态查询 UI
├── macos/                        # 平台工程（含定制 entitlements：关沙盒、开出站网），已提交
└── tool/
    └── e2e_connect.dart          # 端到端可用性：经 daemon 真连服务端拿下发地址
```

## 一、编译 candy-service（在仓库根 `candy/`）

daemon 就是核心自带的 `candy-service` 可执行文件。本机开发用普通动态构建即可：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCANDY_NETSTACK=ON
cmake --build build --target candy-service
# 产物：build/candy-service/candy-service
```

`CANDY_NETSTACK=ON` 提供 userspace lwIP 栈；桌面 userspace 模式必需。

> **发布务必用静态构建**（见第四节）。动态构建的产物硬链了 Homebrew 的 openssl/Poco，
> 且 minos 跟随构建机系统；在更旧的 macOS 上会因依赖缺失或版本过高被系统直接拒绝启动。
> 静态构建从源码编 OpenSSL+Poco 进二进制、下限锁 11.0，产物自包含、跨系统可跑：
>
> ```bash
> export CFLAGS="-mmacosx-version-min=11.0"; export LDFLAGS="-mmacosx-version-min=11.0"
> cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
>   -DCANDY_STATIC=1 -DTARGET_OPENSSL=darwin64-arm64-cc \
>   -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCANDY_NETSTACK=ON
> cmake --build build --target candy-service
> # 产物：build/bin/candy-service（otool -L 应只剩 /usr/lib 系统库）
> ```

## 二、本机开发运行

需要本机装有完整 Xcode + CocoaPods（`flutter build/run macos` 依赖）。

```bash
cd clients/flutter
flutter pub get
# 开发期把 build 产物路径告诉安装器，免去把二进制塞进 .app：
export CANDY_SERVICE_BIN=/abs/path/to/build/candy-service/candy-service
flutter run -d macos
```

填 WebSocket 地址与密码，点“连接”：
- daemon 未安装时会弹一次系统授权框，把 `candy-service` 装成 LaunchDaemon；
- 之后 GUI 通过 `localhost:26817` 驱动它建 utun，状态卡每 2s 轮询显示分配到的地址。

> `macos/` 平台工程（含定制 entitlements）已提交，**不要**再 `flutter create`，会覆盖定制。

## 三、无 GUI 验证可用性（不依赖 Xcode）

本机若缺完整 Xcode 无法 `flutter build macos`，可用纯 Dart 脚本验证「Dart → daemon →
真实连接」这条核心链路（与 GUI 共用同一套 `CandyService` API）。

先以 root 起 daemon（建 utun 需特权），再跑脚本：

```bash
# 1) root 起 daemon
sudo ./build/candy-service/candy-service --bind=127.0.0.1:26817 --loglevel=info

# 2) 另开终端：作为 DHCP 客户端连接，拿到服务端下发地址即证明鉴权往返成功
dart run tool/e2e_connect.dart <ws地址> <密码> [期望地址前缀]
```

期望输出 `E2E_OK`。

## 四、发布打包（GitHub Actions）

`.github/workflows/flutter.yaml` 的 macOS job：

1. `brew install ninja`，**静态**编 `candy-service`（`-DCANDY_STATIC=1` +
   `-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0` + `CFLAGS/LDFLAGS=-mmacosx-version-min=11.0`），
   从源码把 OpenSSL+Poco 静态链进二进制，产物只依赖系统库、下限锁 11.0；
2. `flutter build macos --release` 出 `.app`（用仓库里已提交的 `macos/` 工程与 entitlements,
   **不** `flutter create`）；
3. 把 `build/bin/candy-service` 塞进 `<App>.app/Contents/Resources/`。静态产物自包含，
   **无需**内嵌任何 Frameworks/dylib，也无需改写 rpath；
4. candy-service + 整包 ad-hoc 重签名，`hdiutil` 打成 DMG 上传。`release published` 时挂到 Release。

安装器 `daemon_installer.dart` 的 `locateBundledBinary()` 打包期会从
`<App>.app/Contents/Resources/candy-service` 找到随包二进制，提权拷到
`/Library/Application Support/Candy/` 并注册 LaunchDaemon（单文件拷贝，无依赖搬运）。

> **打开 DMG 里的 app 提示“已损坏，无法打开”**：因未做 Apple 公证（无开发者账号），
> 经浏览器下载的 app 会被打上隔离属性（`com.apple.quarantine`），Gatekeeper 误报“已损坏”。
> app 已在 CI 里 ad-hoc 重签名，只需清除隔离属性即可打开：
>
> ```bash
> # 把 app 拖进“应用程序”后执行（路径按实际调整）
> xattr -cr /Applications/candy.app
> ```

## 说明与后续

- 桌面三端零付费账号、零签名：本机免签直跑，分发靠右键打开 / Gatekeeper “仍要打开”。
- **Windows / Linux**：新架构的提权安装（Windows UAC/服务、Linux pkexec/setcap）与平台
  工程尚未实现，CI 里旧的 FFI 版本 job 已移除。待其提权方案设计验证后再补。
- **iOS**：必须走 NetworkExtension，需付费开发者账号，暂不在本客户端范围内。
