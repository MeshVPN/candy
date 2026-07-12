// SPDX-License-Identifier: MIT
// candy Flutter 桌面客户端骨架：连接 / 断开 / 状态查询最小闭环。
import 'dart:async';
import 'package:flutter/material.dart';

import 'candy_bindings.dart' show CandyResult;
import 'candy_service.dart';

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

  late final CandyService _candy;
  String _libError = '';
  String _version = '-';

  final _wsCtrl = TextEditingController(text: 'wss://canet.buhuoo.com/demo');
  final _pwdCtrl = TextEditingController(text: 'demo-password');
  final _tunCtrl = TextEditingController(text: '');

  bool _connected = false;
  String _statusText = '未连接';
  Timer? _statusTimer;

  @override
  void initState() {
    super.initState();
    try {
      _candy = CandyService.load();
      _version = _candy.version();
    } catch (e) {
      _libError = '动态库加载失败：$e';
    }
  }

  @override
  void dispose() {
    _statusTimer?.cancel();
    _wsCtrl.dispose();
    _pwdCtrl.dispose();
    _tunCtrl.dispose();
    super.dispose();
  }

  void _connect() {
    final config = CandyConfig(
      websocket: _wsCtrl.text.trim(),
      password: _pwdCtrl.text,
      tun: _tunCtrl.text.trim(),
      userspaceStack: true,
    );
    final rc = _candy.start(_instanceId, config);
    if (rc == CandyResult.ok) {
      setState(() {
        _connected = true;
        _statusText = '正在连接…';
      });
      _startStatusPolling();
    } else {
      _toast('启动失败，错误码 $rc');
    }
  }

  void _disconnect() {
    final rc = _candy.stop(_instanceId);
    _statusTimer?.cancel();
    setState(() {
      _connected = false;
      _statusText = rc == CandyResult.ok ? '已断开' : '断开异常，错误码 $rc';
    });
  }

  void _startStatusPolling() {
    _statusTimer?.cancel();
    _statusTimer = Timer.periodic(const Duration(seconds: 2), (_) {
      final st = _candy.status(_instanceId);
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
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(msg)));
  }

  @override
  Widget build(BuildContext context) {
    if (_libError.isNotEmpty) {
      return Scaffold(
        appBar: AppBar(title: const Text('Candy')),
        body: Center(child: Text(_libError)),
      );
    }
    return Scaffold(
      appBar: AppBar(title: Text('Candy  (core $_version)')),
      body: Padding(
        padding: const EdgeInsets.all(24),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            TextField(
              controller: _wsCtrl,
              enabled: !_connected,
              decoration: const InputDecoration(
                labelText: 'WebSocket 地址',
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _pwdCtrl,
              enabled: !_connected,
              obscureText: true,
              decoration: const InputDecoration(
                labelText: '密码',
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _tunCtrl,
              enabled: !_connected,
              decoration: const InputDecoration(
                labelText: 'TUN 地址（留空由服务端分配）',
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 20),
            FilledButton(
              onPressed: _connected ? _disconnect : _connect,
              child: Text(_connected ? '断开' : '连接'),
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
