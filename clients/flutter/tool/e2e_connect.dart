// SPDX-License-Identifier: MIT
// 端到端可用性测试：不依赖 Flutter/Xcode/GUI，直接走 lib/ 的 CandyService
// 业务封装（GUI 用的同一套 API）跑通「连接 → 拿到服务端下发地址 → 断开」全闭环。
//
// 与 ffi_smoke.dart 的区别：smoke 只验静态只读接口；本测试真起连接线程、
// 真跟一个 candy 服务端做 WebSocket 往返，用「地址来自服务端 DHCP 地址池」
// 作为「客户端可用」的铁证。全程无需 root（地址在建 tun 设备前就已拿到）。
//
// 用法（在 clients/flutter/ 下）：
//   dart run tool/e2e_connect.dart <动态库绝对路径> <ws地址> <密码> [期望地址前缀]
// 例：
//   dart run tool/e2e_connect.dart \
//     /abs/build-mac/libcandy_ffi.dylib ws://127.0.0.1:18899 e2e-secret 10.99.0.
import 'dart:io';

import '../lib/candy_service.dart';
import '../lib/candy_bindings.dart' show CandyResult;

Future<void> main(List<String> args) async {
  if (args.length < 3) {
    stderr.writeln('用法: dart run tool/e2e_connect.dart '
        '<动态库路径> <ws地址> <密码> [期望地址前缀]');
    exit(2);
  }
  final libPath = args[0];
  final ws = args[1];
  final password = args[2];
  final expectPrefix = args.length >= 4 ? args[3] : '';
  const id = 'e2e';

  print('== candy 客户端 e2e 可用性测试 ==');
  print('动态库: $libPath');
  print('服务端: $ws');

  final candy = CandyService.load(libPath);
  print('candy_version() = ${candy.version()}');

  // DHCP 客户端：不配静态 tun，交由服务端从地址池分配。vmac 决定分配到的地址。
  final vmac = candy.createVmac();
  if (vmac.isEmpty) {
    stderr.writeln('E2E_FAIL: create_vmac 返回空');
    exit(1);
  }
  print('candy_create_vmac() = $vmac');

  final config = CandyConfig(
    websocket: ws,
    password: password,
    tun: '', // 留空 => 走 DHCP，地址由服务端下发
    expt: '0.0.0.0/0', // 与 CLI 默认一致：请求任意地址
    vmac: vmac,
    userspaceStack: true,
  );

  final rc = candy.start(id, config);
  if (rc != CandyResult.ok) {
    stderr.writeln('E2E_FAIL: candy_start 返回 $rc');
    exit(1);
  }
  print('candy_start() = OK（连接线程已启动，开始轮询状态）');

  // 轮询状态：地址非空即证明与服务端完成了 WebSocket 鉴权往返并拿到下发地址。
  String address = '';
  const maxTries = 20; // 最多等 ~10s
  for (var i = 1; i <= maxTries; i++) {
    await Future<void>.delayed(const Duration(milliseconds: 500));
    final st = candy.status(id);
    final addr = (st?['address'] as String?) ?? '';
    print('  [$i/$maxTries] status.address = '
        '${addr.isEmpty ? "(空，连接中…)" : addr}');
    if (addr.isNotEmpty) {
      address = addr;
      break;
    }
  }

  // 无论成败都要停实例，避免残留后台线程。
  final stopRc = candy.stop(id);
  print('candy_stop() = ${stopRc == CandyResult.ok ? "OK" : "错误码 $stopRc"}');

  if (address.isEmpty) {
    stderr.writeln('E2E_FAIL: 超时未拿到服务端下发地址（连接未建立）');
    exit(1);
  }
  if (expectPrefix.isNotEmpty && !address.startsWith(expectPrefix)) {
    stderr.writeln('E2E_FAIL: 地址 $address 不在期望地址池 $expectPrefix*');
    exit(1);
  }
  print('E2E_OK: 已连接，服务端下发地址 = $address');
}
