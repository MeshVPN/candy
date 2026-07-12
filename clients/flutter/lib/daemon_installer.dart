// SPDX-License-Identifier: MIT
// 特权 daemon 安装器（纯 Dart，无 Swift/ObjC）。
//
// macOS 建 utun 是特权操作，GUI 进程（普通用户）无权直接建网卡。方案参照
// ClashX / Tunnelblick：把 candy-service 装成 root 的 LaunchDaemon 常驻，
// GUI 通过 localhost HTTP 驱动它。首次安装时用 osascript 触发一次系统授权框，
// 之后 daemon 开机自启、永久免密。
//
// 所有特权操作（拷贝二进制、写 plist、launchctl load）合并成一条 shell 命令，
// 交给 `osascript ... with administrator privileges` 一次性执行，只弹一次密码框。
import 'dart:io';

/// daemon 安装相关的常量与路径。
class DaemonPaths {
  /// LaunchDaemon 标签，也是 plist 文件名与 launchctl 的 service 名。
  static const label = 'org.candy.service';

  /// daemon 二进制安装目标目录（root 拥有）。
  static const installDir = '/Library/Application Support/Candy';

  /// 安装后的 daemon 二进制绝对路径。
  static const installedBinary = '$installDir/candy-service';

  /// LaunchDaemon plist 路径。
  static const plistPath = '/Library/LaunchDaemons/$label.plist';

  /// daemon 日志目录。
  static const logDir = '$installDir/logs';

  /// daemon 绑定地址与端口。
  static const bindHost = '127.0.0.1';
  static const bindPort = 26817;
}

class DaemonInstaller {
  /// 定位随 App 分发的 candy-service 源二进制。
  /// - 打包后：<App>.app/Contents/Resources/candy-service
  /// - 开发期：允许通过环境变量 CANDY_SERVICE_BIN 指定 build 产物绝对路径。
  static String? locateBundledBinary() {
    final override = Platform.environment['CANDY_SERVICE_BIN'];
    if (override != null && File(override).existsSync()) {
      return override;
    }
    final exeDir = File(Platform.resolvedExecutable).parent.path;
    // macOS: Contents/MacOS/<exe> -> Contents/Resources/candy-service
    final bundled = '$exeDir/../Resources/candy-service';
    if (File(bundled).existsSync()) {
      return bundled;
    }
    return null;
  }

  /// daemon 是否已安装（plist 与二进制都在）。
  static bool isInstalled() {
    return File(DaemonPaths.plistPath).existsSync() &&
        File(DaemonPaths.installedBinary).existsSync();
  }

  /// 生成 LaunchDaemon plist 内容。KeepAlive 保证崩溃自动拉起，
  /// RunAtLoad 保证 load 后立即启动、开机自启。
  static String buildPlist() {
    return '''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>${DaemonPaths.label}</string>
	<key>ProgramArguments</key>
	<array>
		<string>${DaemonPaths.installedBinary}</string>
		<string>--bind=${DaemonPaths.bindHost}:${DaemonPaths.bindPort}</string>
		<string>--logdir=${DaemonPaths.logDir}</string>
		<string>--loglevel=info</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>KeepAlive</key>
	<true/>
</dict>
</plist>
''';
  }

  /// 安装并启动 daemon。会弹出一次系统管理员授权框。
  /// 成功返回 null；失败返回错误描述（用户取消授权也算失败）。
  static Future<String?> install() async {
    final srcBinary = locateBundledBinary();
    if (srcBinary == null) {
      return '找不到 candy-service 二进制。开发期请设置环境变量 '
          'CANDY_SERVICE_BIN 指向 build 产物，或将其放入 App 的 Resources 目录。';
    }

    // 把 plist 先写到临时文件，再由特权脚本拷入 /Library/LaunchDaemons。
    final tmpPlist = File(
        '${Directory.systemTemp.path}/${DaemonPaths.label}.plist');
    try {
      await tmpPlist.writeAsString(buildPlist());
    } catch (e) {
      return '写入临时 plist 失败: $e';
    }

    // 合并成一条特权 shell 命令，只弹一次授权框。
    // 若已加载旧实例先卸载，避免 load 冲突（bootout 失败忽略）。
    final script = _buildInstallScript(
      srcBinary: srcBinary,
      tmpPlist: tmpPlist.path,
    );

    final result = await _runWithPrivilege(script);
    try {
      await tmpPlist.delete();
    } catch (_) {}
    return result;
  }

  /// 卸载 daemon（停止、移除 plist 与二进制）。同样弹一次授权框。
  static Future<String?> uninstall() async {
    const script = '''
launchctl bootout system/${DaemonPaths.label} 2>/dev/null || true
rm -f '${DaemonPaths.plistPath}'
rm -rf '${DaemonPaths.installDir}'
''';
    return _runWithPrivilege(script);
  }

  static String _buildInstallScript({
    required String srcBinary,
    required String tmpPlist,
  }) {
    return '''
mkdir -p '${DaemonPaths.installDir}'
mkdir -p '${DaemonPaths.logDir}'
cp '$srcBinary' '${DaemonPaths.installedBinary}'
chmod 755 '${DaemonPaths.installedBinary}'
chown root:wheel '${DaemonPaths.installedBinary}'
cp '$tmpPlist' '${DaemonPaths.plistPath}'
chown root:wheel '${DaemonPaths.plistPath}'
chmod 644 '${DaemonPaths.plistPath}'
launchctl bootout system/${DaemonPaths.label} 2>/dev/null || true
launchctl bootstrap system '${DaemonPaths.plistPath}'
''';
  }

  /// 通过 osascript 以管理员权限执行 shell 脚本。
  /// 用户点“好”输入密码即执行；点“取消”返回 -128，此处转成友好错误。
  static Future<String?> _runWithPrivilege(String shellScript) async {
    // 脚本作为单一 do shell script 执行；转义双引号与反斜杠。
    final escaped = shellScript
        .replaceAll('\\', '\\\\')
        .replaceAll('"', '\\"');
    final osaScript =
        'do shell script "$escaped" with administrator privileges';

    try {
      final result = await Process.run('osascript', ['-e', osaScript]);
      if (result.exitCode == 0) {
        return null;
      }
      final err = (result.stderr as String).trim();
      if (err.contains('-128')) {
        return '已取消授权';
      }
      return err.isEmpty ? '安装失败（退出码 ${result.exitCode}）' : err;
    } catch (e) {
      return '执行授权命令失败: $e';
    }
  }
}
