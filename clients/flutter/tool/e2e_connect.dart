// SPDX-License-Identifier: MIT
// 端到端可用性测试：不依赖 Flutter/Xcode/GUI，直接走 lib/ 的 CandyService
// 业务封装（GUI 用的同一套 API）跑通「连接 → 拿到服务端下发地址 → 断开」全闭环。
//
// 新架构：CandyService 通过 HTTP 与 root daemon (candy-service) 通信。
// 运行前需先手动以 root 起 daemon：
//   sudo /abs/build-mac/candy-service/candy-service --bind=127.0.0.1:26817 --loglevel=info
//
// 用法（在 clients/flutter/ 下）：
//   dart run tool/e2e_connect.dart <ws地址> <密码> [期望地址前缀]
// 例：
//   dart run tool/e2e_connect.dart ws://127.0.0.1:18899 e2e-secret 10.99.0.
import 'dart:io';

import '../lib/candy_service.dart';
import '../lib/candy_daemon_client.dart';

Future<void> main(List<String> args) async {
  if (args.length < 2) {
    stderr.writeln('用法: dart run tool/e2e_connect.dart '
        '<ws地址> <密码> [期望地址前缀]');
    exit(2);
  }
  final ws = args[0];
  final password = args[1];
  final expectPrefix = args.length >= 3 ? args[2] : '';
  const id = 'e2e';

  print('== candy 客户端 e2e 可用性测试（HTTP daemon）==');
  print('服务端: $ws');

  final candy = CandyService();

  if (!await candy.isDaemonAlive()) {
    stderr.writeln('E2E_FAIL: daemon 未在线，请先以 root 启动 candy-service');
    exit(1);
  }
  print('daemon 在线');

  // DHCP 客户端：不配静态 tun，交由服务端从地址池分配。vmac 决定分配到的地址。
  final vmac = CandyService.createVmac();
  print('vmac = $vmac');

  final config = CandyConfig(
    websocket: ws,
    password: password,
    tun: '', // 留空 => 走 DHCP，地址由服务端下发
    expt: '0.0.0.0/0', // 与 CLI 默认一致：请求任意地址
    vmac: vmac,
  );

  try {
    await candy.start(id, config);
  } on CandyDaemonException catch (e) {
    stderr.writeln('E2E_FAIL: start 失败: ${e.message}');
    exit(1);
  }
  print('start() = OK（daemon 已起连接线程，开始轮询状态）');

  // 轮询状态：地址非空即证明与服务端完成了 WebSocket 鉴权往返并拿到下发地址。
  String address = '';
  const maxTries = 20; // 最多等 ~10s
  for (var i = 1; i <= maxTries; i++) {
    await Future<void>.delayed(const Duration(milliseconds: 500));
    final st = await candy.status(id);
    final addr = (st?['address'] as String?) ?? '';
    print('  [$i/$maxTries] status.address = '
        '${addr.isEmpty ? "(空，连接中…)" : addr}');
    if (addr.isNotEmpty) {
      address = addr;
      break;
    }
  }

  // 无论成败都要停实例，避免残留后台线程。
  await candy.stop(id);
  print('stop() = OK');

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
