// SPDX-License-Identifier: MIT
// daemon HTTP 通信层：与 candy-service 的 REST 接口一一对应。
// candy-service 以 root 常驻，暴露 /api/run /api/status /api/shutdown，
// 默认绑定 localhost:26817（仅本机可连）。此层只做 HTTP 收发与 JSON 编解码，
// 不含业务逻辑；业务封装见 candy_service.dart。
import 'dart:async';
import 'dart:convert';
import 'dart:io';

/// daemon 返回的错误。message 为 daemon 响应里的 message 字段或传输层异常描述。
class CandyDaemonException implements Exception {
  final String message;
  const CandyDaemonException(this.message);
  @override
  String toString() => 'CandyDaemonException: $message';
}

/// 与 candy-service 通信的底层 HTTP 客户端。
class CandyDaemonClient {
  final String host;
  final int port;
  final Duration timeout;

  const CandyDaemonClient({
    this.host = '127.0.0.1',
    this.port = 26817,
    this.timeout = const Duration(seconds: 5),
  });

  /// 探测 daemon 是否在线。任意能建立连接并返回响应即视为在线。
  /// 用不存在的实例查 status，daemon 在线时会回 "id does not exist"。
  Future<bool> isAlive() async {
    try {
      await _post('/api/status', {'id': '__probe__'});
      return true;
    } catch (_) {
      return false;
    }
  }

  /// 启动一个实例。config 必须包含 candy-service 要求的全部必填字段。
  /// daemon 侧成功返回 {"message":"success"}；重复 id 返回 "id already exists"。
  Future<void> run(String id, Map<String, dynamic> config) async {
    final resp = await _post('/api/run', {'id': id, 'config': config});
    _ensureSuccess(resp);
  }

  /// 查询实例状态。返回 status 对象（如 {"address":"10.33.0.221/24"}）；
  /// 实例不存在或尚无状态时返回 null。
  Future<Map<String, dynamic>?> status(String id) async {
    final resp = await _post('/api/status', {'id': id});
    final message = resp['message'];
    if (message == 'success') {
      final status = resp['status'];
      if (status is Map<String, dynamic>) {
        return status;
      }
      return <String, dynamic>{};
    }
    // "id does not exist" / "unable to get status" 均视为无状态。
    return null;
  }

  /// 停止实例。实例不存在时 daemon 返回 "id does not exist"，此处不视为错误。
  Future<void> shutdown(String id) async {
    await _post('/api/shutdown', {'id': id});
  }

  Future<Map<String, dynamic>> _post(String path, Map<String, dynamic> body) async {
    final client = HttpClient()..connectionTimeout = timeout;
    try {
      final req = await client
          .postUrl(Uri.parse('http://$host:$port$path'))
          .timeout(timeout);
      req.headers.contentType = ContentType.json;
      req.add(utf8.encode(jsonEncode(body)));
      final resp = await req.close().timeout(timeout);
      final text = await resp.transform(utf8.decoder).join().timeout(timeout);
      if (resp.statusCode != HttpStatus.ok) {
        throw CandyDaemonException('HTTP ${resp.statusCode}: $text');
      }
      final decoded = jsonDecode(text);
      if (decoded is Map<String, dynamic>) {
        return decoded;
      }
      throw CandyDaemonException('响应格式非法: $text');
    } on SocketException catch (e) {
      throw CandyDaemonException('无法连接 daemon ($host:$port): ${e.message}');
    } on TimeoutException {
      throw CandyDaemonException('连接 daemon 超时 ($host:$port)');
    } finally {
      client.close(force: true);
    }
  }

  void _ensureSuccess(Map<String, dynamic> resp) {
    final message = resp['message'];
    if (message != 'success') {
      throw CandyDaemonException(message?.toString() ?? '未知错误');
    }
  }
}
