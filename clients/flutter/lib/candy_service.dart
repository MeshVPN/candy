// SPDX-License-Identifier: MIT
// 业务封装层：把 daemon HTTP 调用包装成对 UI 友好的 Dart API。
// 负责 vmac 生成、config 补全（daemon 侧 12 个字段无条件读取，缺一即崩）、
// 状态解析。底层通信见 candy_daemon_client.dart。
import 'dart:math';

import 'candy_daemon_client.dart';

/// candy 客户端连接配置，对应 candy-service /api/run 的 config 字段。
/// 注意：daemon 侧对 name/password/websocket/tun/vmac/expt/stun/discovery/
/// route/mtu/port/localhost 全部用 getValue 无条件读取，toJson 必须全量输出。
class CandyConfig {
  final String name;
  final String password;
  final String websocket;
  final String tun;
  final String vmac;
  final String expt;
  final String stun;
  final int discovery;
  final int route;
  final int mtu;
  final int port;
  final String localhost;
  final bool userspaceStack;

  const CandyConfig({
    this.name = '',
    this.password = '',
    required this.websocket,
    this.tun = '',
    this.vmac = '',
    this.expt = '',
    this.stun = '',
    this.discovery = 300,
    this.route = 5,
    this.mtu = 1400,
    this.port = 0,
    this.localhost = '',
    this.userspaceStack = false,
  });

  CandyConfig copyWith({String? vmac}) => CandyConfig(
        name: name,
        password: password,
        websocket: websocket,
        tun: tun,
        vmac: vmac ?? this.vmac,
        expt: expt,
        stun: stun,
        discovery: discovery,
        route: route,
        mtu: mtu,
        port: port,
        localhost: localhost,
        userspaceStack: userspaceStack,
      );

  Map<String, dynamic> toJson() => {
        'name': name,
        'password': password,
        'websocket': websocket,
        'tun': tun,
        'vmac': vmac,
        'expt': expt,
        'stun': stun,
        'discovery': discovery,
        'route': route,
        'mtu': mtu,
        'port': port,
        'localhost': localhost,
        'userspace-stack': userspaceStack,
      };
}

/// candy 的高层业务封装。通过 HTTP 与 root daemon 通信驱动 candy 核心。
class CandyService {
  final CandyDaemonClient _client;

  CandyService({CandyDaemonClient? client})
      : _client = client ?? const CandyDaemonClient();

  /// 生成一个虚拟 MAC：32 位十六进制字符串（等价核心 randomHexString(16)）。
  /// daemon 侧 VMac 要求长度 >= 16 字节，32 hex 字符满足。
  static String createVmac() {
    final rnd = Random.secure();
    final sb = StringBuffer();
    for (var i = 0; i < 32; i++) {
      sb.write(rnd.nextInt(16).toRadixString(16));
    }
    return sb.toString();
  }

  /// daemon 是否在线。
  Future<bool> isDaemonAlive() => _client.isAlive();

  /// 启动实例（非阻塞）。若 config.vmac 为空会自动补一个。
  /// 失败抛 CandyDaemonException。
  Future<void> start(String id, CandyConfig config) async {
    final filled =
        config.vmac.isEmpty ? config.copyWith(vmac: createVmac()) : config;
    await _client.run(id, filled.toJson());
  }

  /// 停止实例（幂等）。
  Future<void> stop(String id) => _client.shutdown(id);

  /// 查询状态。无实例返回 null；有则返回 map（如 {"address":"..."}）。
  Future<Map<String, dynamic>?> status(String id) => _client.status(id);
}
