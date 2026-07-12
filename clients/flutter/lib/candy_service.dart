// SPDX-License-Identifier: MIT
// 业务封装层：把 dart:ffi 裸指针调用包装成对 UI 友好的 Dart API。
// 负责字符串编解码、native 内存释放、JSON 组装与解析。
import 'dart:convert';
import 'dart:ffi';
import 'package:ffi/ffi.dart';

import 'candy_bindings.dart';

/// candy 客户端连接配置，对应 capi.h 中 candy_start 的 json 字段。
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
    required this.password,
    required this.websocket,
    this.tun = '',
    this.vmac = '',
    this.expt = '',
    this.stun = '',
    this.discovery = 0,
    this.route = 0,
    this.mtu = 1400,
    this.port = 0,
    this.localhost = '',
    this.userspaceStack = true,
  });

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

/// candy SDK 的高层封装。单例持有 FFI 绑定。
class CandyService {
  final CandyBindings _bindings;

  CandyService._(this._bindings);

  factory CandyService.load([String? libraryPath]) =>
      CandyService._(CandyBindings.open(libraryPath));

  /// 核心库版本号。
  String version() {
    final ptr = _bindings.candyVersion();
    // candy_version 返回进程内静态字符串，不能释放。
    return ptr.toDartString();
  }

  /// 生成一个虚拟 MAC。
  String createVmac() {
    final ptr = _bindings.candyCreateVmac();
    if (ptr == nullptr) {
      return '';
    }
    try {
      return ptr.toDartString();
    } finally {
      _bindings.candyStringFree(ptr);
    }
  }

  /// 启动实例（非阻塞）。返回 CandyResult 错误码。
  int start(String id, CandyConfig config) {
    final idPtr = id.toNativeUtf8();
    final jsonPtr = jsonEncode(config.toJson()).toNativeUtf8();
    try {
      return _bindings.candyStart(idPtr, jsonPtr);
    } finally {
      calloc.free(idPtr);
      calloc.free(jsonPtr);
    }
  }

  /// 停止实例（同步，通常 1s 内返回）。
  int stop(String id) {
    final idPtr = id.toNativeUtf8();
    try {
      return _bindings.candyStop(idPtr);
    } finally {
      calloc.free(idPtr);
    }
  }

  /// 查询状态。无实例返回 null；有则返回解析后的 map（如 {"address": "..."}）。
  Map<String, dynamic>? status(String id) {
    final idPtr = id.toNativeUtf8();
    try {
      final resPtr = _bindings.candyStatus(idPtr);
      if (resPtr == nullptr) {
        return null;
      }
      try {
        final raw = resPtr.toDartString();
        if (raw.isEmpty) {
          return {};
        }
        return jsonDecode(raw) as Map<String, dynamic>;
      } finally {
        _bindings.candyStringFree(resPtr);
      }
    } finally {
      calloc.free(idPtr);
    }
  }

  /// 注入系统 tun fd（移动端第二阶段；桌面端不需要，当前返回 unimplemented）。
  int setTunFd(String id, int fd) {
    final idPtr = id.toNativeUtf8();
    try {
      return _bindings.candySetTunFd(idPtr, fd);
    } finally {
      calloc.free(idPtr);
    }
  }
}
