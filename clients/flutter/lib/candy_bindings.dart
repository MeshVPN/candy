// SPDX-License-Identifier: MIT
// dart:ffi 绑定层：与 candy/include/candy/capi.h 的纯 C ABI 一一对应。
// 仅做类型映射与符号查找，不含业务逻辑；业务封装见 candy_service.dart。
import 'dart:ffi';
import 'dart:io';
import 'package:ffi/ffi.dart';

// —— C 函数签名（native）——
typedef _CandyStartNative = Int32 Function(
    Pointer<Utf8> id, Pointer<Utf8> json);
typedef _CandyStopNative = Int32 Function(Pointer<Utf8> id);
typedef _CandyStatusNative = Pointer<Utf8> Function(Pointer<Utf8> id);
typedef _CandyStringFreeNative = Void Function(Pointer<Utf8> s);
typedef _CandyVersionNative = Pointer<Utf8> Function();
typedef _CandyCreateVmacNative = Pointer<Utf8> Function();
typedef _CandySetTunFdNative = Int32 Function(Pointer<Utf8> id, Int32 fd);

// —— Dart 侧签名 ——
typedef _CandyStartDart = int Function(Pointer<Utf8> id, Pointer<Utf8> json);
typedef _CandyStopDart = int Function(Pointer<Utf8> id);
typedef _CandyStatusDart = Pointer<Utf8> Function(Pointer<Utf8> id);
typedef _CandyStringFreeDart = void Function(Pointer<Utf8> s);
typedef _CandyVersionDart = Pointer<Utf8> Function();
typedef _CandyCreateVmacDart = Pointer<Utf8> Function();
typedef _CandySetTunFdDart = int Function(Pointer<Utf8> id, int fd);

/// 与 capi.h 的 candy_result 枚举保持一致。
class CandyResult {
  static const int ok = 0;
  static const int invalidArg = -1;
  static const int alreadyRunning = -2;
  static const int notFound = -3;
  static const int badConfig = -4;
  static const int unimplemented = -5;
  static const int internal = -6;
}

/// 低层 FFI 绑定：持有 DynamicLibrary 并暴露已解析的函数指针。
class CandyBindings {
  final DynamicLibrary _lib;

  late final _CandyStartDart candyStart =
      _lib.lookupFunction<_CandyStartNative, _CandyStartDart>('candy_start');
  late final _CandyStopDart candyStop =
      _lib.lookupFunction<_CandyStopNative, _CandyStopDart>('candy_stop');
  late final _CandyStatusDart candyStatus =
      _lib.lookupFunction<_CandyStatusNative, _CandyStatusDart>('candy_status');
  late final _CandyStringFreeDart candyStringFree =
      _lib.lookupFunction<_CandyStringFreeNative, _CandyStringFreeDart>(
          'candy_string_free');
  late final _CandyVersionDart candyVersion = _lib
      .lookupFunction<_CandyVersionNative, _CandyVersionDart>('candy_version');
  late final _CandyCreateVmacDart candyCreateVmac =
      _lib.lookupFunction<_CandyCreateVmacNative, _CandyCreateVmacDart>(
          'candy_create_vmac');
  late final _CandySetTunFdDart candySetTunFd =
      _lib.lookupFunction<_CandySetTunFdNative, _CandySetTunFdDart>(
          'candy_set_tun_fd');

  CandyBindings(this._lib);

  /// 按平台加载动态库。桌面端默认从可执行文件同目录加载；
  /// 传入 path 可指定绝对路径（打包放 Frameworks、或测试指定构建产物）。
  factory CandyBindings.open([String? path]) {
    return CandyBindings(DynamicLibrary.open(path ?? _libraryPath()));
  }

  static String _libraryPath() {
    // 打包后的桌面 App 工作目录通常是 "/"，按裸文件名 dlopen 会失败，需相对
    // 可执行文件定位：macOS 放 Contents/Frameworks/，Linux 放同级 lib/，
    // Windows 放 exe 同目录。找不到再回退裸名（交系统搜索路径 / 开发期同目录）。
    final exeDir = File(Platform.resolvedExecutable).parent.path;
    String candidate;
    String leaf;
    if (Platform.isMacOS) {
      leaf = 'libcandy_ffi.dylib';
      candidate = '$exeDir/../Frameworks/$leaf';
    } else if (Platform.isWindows) {
      leaf = 'candy_ffi.dll';
      candidate = '$exeDir\\$leaf';
    } else if (Platform.isLinux) {
      leaf = 'libcandy_ffi.so';
      candidate = '$exeDir/lib/$leaf';
    } else {
      throw UnsupportedError('unsupported platform for candy ffi');
    }
    return File(candidate).existsSync() ? candidate : leaf;
  }
}
