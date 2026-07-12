// SPDX-License-Identifier: MIT
// candy Flutter 桌面客户端：连接 / 断开 / 状态查询最小闭环。
// 架构：GUI(普通用户) --HTTP--> candy-service(root daemon) --> candy 核心建 utun。
// 首次连接若 daemon 未安装，会触发一次系统授权把 daemon 装成 LaunchDaemon。
import 'dart:async';
import 'package:flutter/material.dart';

import 'candy_daemon_client.dart';
import 'candy_service.dart';
import 'daemon_installer.dart';

void main() {
  runApp(const CandyApp());
}

class CandyApp extends StatelessWidget {
  const CandyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Candy',
      theme: ThemeData(colorSchemeSeed: Colors.teal, useMaterial3: true),
      home: const HomePage(),
    );
  }
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  static const _instanceId = 'default';

  final _candy = CandyService();

  final _wsCtrl = TextEditingController(text: 'wss://canet.buhuoo.com/demo');
  final _pwdCtrl = TextEditingController(text: 'demo-password');
  final _tunCtrl = TextEditingController(text: '');

  bool _connected = false;
  bool _busy = false;
  String _statusText = '未连接';
  Timer? _statusTimer;

  @override
  void dispose() {
    _statusTimer?.cancel();
    _wsCtrl.dispose();
    _pwdCtrl.dispose();
    _tunCtrl.dispose();
    super.dispose();
  }

  /// 确保 daemon 在线：已在线直接用；未安装则提权安装；已装未起则等它起来。
  /// 返回 true 表示 daemon 可用。
  Future<bool> _ensureDaemon() async {
    if (await _candy.isDaemonAlive()) return true;

    if (!DaemonInstaller.isInstalled()) {
      setState(() => _statusText = '首次使用，正在申请管理员授权安装后台服务…');
      final err = await DaemonInstaller.install();
      if (err != null) {
        _toast('安装后台服务失败：$err');
        return false;
      }
    }

    // 等待 daemon 起来（launchd 拉起需要一点时间）。
    setState(() => _statusText = '正在启动后台服务…');
    for (var i = 0; i < 20; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 300));
      if (await _candy.isDaemonAlive()) return true;
    }
    _toast('后台服务未能启动，请检查日志');
    return false;
  }

  Future<void> _connect() async {
    setState(() => _busy = true);
    try {
      if (!await _ensureDaemon()) {
        setState(() => _statusText = '未连接');
        return;
      }

      final config = CandyConfig(
        websocket: _wsCtrl.text.trim(),
        password: _pwdCtrl.text,
        tun: _tunCtrl.text.trim(),
        expt: '0.0.0.0/0',
      );

      try {
        await _candy.start(_instanceId, config);
      } on CandyDaemonException catch (e) {
        _toast('启动失败：${e.message}');
        setState(() => _statusText = '未连接');
        return;
      }

      setState(() {
        _connected = true;
        _statusText = '正在连接…';
      });
      _startStatusPolling();
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _disconnect() async {
    setState(() => _busy = true);
    _statusTimer?.cancel();
    try {
      await _candy.stop(_instanceId);
      setState(() {
        _connected = false;
        _statusText = '已断开';
      });
    } on CandyDaemonException catch (e) {
      setState(() {
        _connected = false;
        _statusText = '断开异常：${e.message}';
      });
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  void _startStatusPolling() {
    _statusTimer?.cancel();
    _statusTimer = Timer.periodic(const Duration(seconds: 2), (_) async {
      final Map<String, dynamic>? st;
      try {
        st = await _candy.status(_instanceId);
      } on CandyDaemonException {
        return;
      }
      if (!mounted) return;
      setState(() {
        if (st == null) {
          _statusText = '实例不存在';
        } else if (st['address'] != null &&
            (st['address'] as String).isNotEmpty) {
          _statusText = '已连接  地址：${st['address']}';
        } else {
          _statusText = '连接中（尚未分配地址）…';
        }
      });
    });
  }

  void _toast(String msg) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(msg)));
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Candy')),
      body: Padding(
        padding: const EdgeInsets.all(24),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            TextField(
              controller: _wsCtrl,
              enabled: !_connected && !_busy,
              decoration: const InputDecoration(
                labelText: 'WebSocket 地址',
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _pwdCtrl,
              enabled: !_connected && !_busy,
              obscureText: true,
              decoration: const InputDecoration(
                labelText: '密码',
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _tunCtrl,
              enabled: !_connected && !_busy,
              decoration: const InputDecoration(
                labelText: 'TUN 地址（留空由服务端分配）',
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 20),
            FilledButton(
              onPressed: _busy
                  ? null
                  : (_connected ? _disconnect : _connect),
              child: _busy
                  ? const SizedBox(
                      height: 18,
                      width: 18,
                      child: CircularProgressIndicator(strokeWidth: 2),
                    )
                  : Text(_connected ? '断开' : '连接'),
            ),
            const SizedBox(height: 24),
            Card(
              child: ListTile(
                leading: Icon(
                  _connected ? Icons.check_circle : Icons.cancel,
                  color: _connected ? Colors.green : Colors.grey,
                ),
                title: const Text('状态'),
                subtitle: Text(_statusText),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
