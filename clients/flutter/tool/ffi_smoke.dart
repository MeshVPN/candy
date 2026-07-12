// SPDX-License-Identifier: MIT
// 纯 Dart FFI 冒烟测试：不依赖 Flutter/Xcode，直接用 dart:ffi 加载编好的
// 动态库并调用只读接口，验证「Dart 复用 native 库」这条链路是通的。
//
// 用法（在 clients/flutter/ 下）：
//   dart run tool/ffi_smoke.dart /绝对路径/libcandy_ffi.dylib
import 'dart:ffi';
import 'dart:io';
import 'package:ffi/ffi.dart';

typedef _VersionNative = Pointer<Utf8> Function();
typedef _VersionDart = Pointer<Utf8> Function();
typedef _CreateVmacNative = Pointer<Utf8> Function();
typedef _CreateVmacDart = Pointer<Utf8> Function();
typedef _StringFreeNative = Void Function(Pointer<Utf8>);
typedef _StringFreeDart = void Function(Pointer<Utf8>);
typedef _StatusNative = Pointer<Utf8> Function(Pointer<Utf8>);
typedef _StatusDart = Pointer<Utf8> Function(Pointer<Utf8>);

void main(List<String> args) {
  if (args.isEmpty) {
    stderr.writeln('用法: dart run tool/ffi_smoke.dart <动态库路径>');
    exit(2);
  }
  final path = args[0];
  print('加载动态库: $path');
  final lib = DynamicLibrary.open(path);

  final version =
      lib.lookupFunction<_VersionNative, _VersionDart>('candy_version');
  final createVmac = lib
      .lookupFunction<_CreateVmacNative, _CreateVmacDart>('candy_create_vmac');
  final stringFree = lib
      .lookupFunction<_StringFreeNative, _StringFreeDart>('candy_string_free');
  final status = lib.lookupFunction<_StatusNative, _StatusDart>('candy_status');

  // 1) candy_version：返回进程内静态字符串，不释放。
  final v = version().toDartString();
  print('candy_version()      = $v');

  // 2) candy_create_vmac：返回堆字符串，需 candy_string_free。
  final vmacPtr = createVmac();
  final vmac = vmacPtr.toDartString();
  stringFree(vmacPtr);
  print('candy_create_vmac()  = $vmac');

  // 3) candy_status(未启动实例)：应返回 NULL（nullptr）。
  final idPtr = 'nonexistent'.toNativeUtf8();
  final stPtr = status(idPtr);
  calloc.free(idPtr);
  print(
      'candy_status(none)   = ${stPtr == nullptr ? "NULL(符合预期)" : stPtr.toDartString()}');

  if (v.isEmpty || vmac.isEmpty) {
    stderr.writeln('SMOKE_FAIL: 返回值为空');
    exit(1);
  }
  print('SMOKE_OK: FFI 链路验证通过');
}
