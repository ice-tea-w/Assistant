"""本地服务：让桌面助手（Electron / WPF / Tauri / Python 均可用）通过 HTTP 调用模块。

设计稿要求"插件化、松耦合、模块崩溃不影响主程序"：模块作为**独立进程**运行，
主程序只通过本机 HTTP + Token 通信；即便模块进程异常退出，重启即可，不影响主程序。

* 默认只监听 ``127.0.0.1``；非回环地址必须显式 ``--allow-remote``（默认拒绝）；
* 每次启动生成随机 token，写入 ``<home>/service.token`` 并**收紧文件 ACL**；
* 校验 ``Host``/``Origin``：挡掉 DNS rebinding 和浏览器页面跨站调用本机端口
  （浏览器能从恶意网页发请求到 127.0.0.1，即使拿不到响应也能触发副作用）；
* 请求体为 JSON：``POST /rpc  {"method": "...", "params": {...}}``；
* 响应：``{"ok": true, "result": ...}`` 或 ``{"ok": false, "error": {...}}``。

生产加固建议（文档里也写了）：把耗时任务改成异步 job + 轮询、必要时改用命名管道/
stdio JSON-RPC 避免本机端口暴露。
"""

from __future__ import annotations

import hmac
import ipaddress
import json
import os
import secrets
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Optional, Union

from . import __version__
from .audit import AuditLog
from .backup import BackupStore
from .config import Config
from .crypto import (
    Password,
    decrypt_file,
    encrypt_file,
    inspect_container,
)
from .errors import FileGuardError, ServiceError
from .fsutil import restrict_acl
from .scan import SensitiveScanner

MAX_BODY = 8 << 20
#: 单个连接读写的超时：防止本机客户端只发一半请求就把线程永久占住
REQUEST_TIMEOUT = 30.0
#: 同时处理的请求上限（ThreadingHTTPServer 默认无上限，可被本机进程轻易耗光线程）
MAX_CONCURRENT_REQUESTS = 16
LOOPBACK_HOSTS = {"127.0.0.1", "::1", "localhost"}
#: 浏览器会带 Origin；桌面助手（原生进程）不会。带 Origin 的请求一律拒绝。
SERVICE_TOKEN_MIN_LEN = 20


def is_loopback_host(host: str) -> bool:
    candidate = (host or "").strip().strip("[]")
    if candidate.lower() in LOOPBACK_HOSTS:
        return True
    try:
        return ipaddress.ip_address(candidate).is_loopback
    except ValueError:
        return False


class Session:
    """一次服务进程内的共享状态（密码只存在内存中）。"""

    def __init__(self, config: Config, password: Password) -> None:
        self.config = config
        self.password = password
        self._audit: Optional[AuditLog] = None
        self._backups: Optional[BackupStore] = None
        self._lock = threading.Lock()

    @property
    def audit(self) -> AuditLog:
        with self._lock:
            if self._audit is None:
                self._audit = AuditLog(
                    self.config.audit_dir,
                    self.password,
                    iterations=self.config.log_iterations,
                )
        return self._audit

    @property
    def backups(self) -> BackupStore:
        with self._lock:
            if self._backups is None:
                self._backups = BackupStore(
                    self.config.backup_dir,
                    self.password,
                    iterations=self.config.iterations,
                    chunk_size=self.config.chunk_size,
                    log_iterations=self.config.log_iterations,
                    keep_versions=self.config.backup_keep,
                )
        return self._backups

    def close(self) -> None:
        self._audit = None
        self._backups = None
        self.password = b""


def ensure_token(home: Path) -> str:
    """读取或创建服务 token；文件权限始终收紧到仅当前用户可读。

    token 是本地服务的唯一凭据：文件一旦被同机其他用户读到，对方就能以本用户
    身份调用 encrypt/decrypt/restore。Windows 上 ``os.chmod`` 不改 ACL，必须用
    ``icacls``（见 fsutil.restrict_acl）。
    """
    token_file = home / "service.token"
    home.mkdir(parents=True, exist_ok=True)
    if token_file.exists():
        try:
            token = token_file.read_text(encoding="utf-8").strip()
        except OSError as exc:
            raise ServiceError(f"无法读取服务 token：{token_file}") from exc
        if len(token) >= SERVICE_TOKEN_MIN_LEN:
            restrict_acl(token_file)
            return token
    token = secrets.token_urlsafe(32)
    try:
        fd = os.open(
            token_file,
            os.O_CREAT | os.O_EXCL | os.O_WRONLY | getattr(os, "O_BINARY", 0),
            0o600,
        )
    except FileExistsError:
        # 并发启动：另一个进程刚建好，直接用它写下的 token
        token = token_file.read_text(encoding="utf-8").strip()
        if len(token) < SERVICE_TOKEN_MIN_LEN:
            raise ServiceError(f"服务 token 文件内容异常：{token_file}")
        restrict_acl(token_file)
        return token
    try:
        os.write(fd, token.encode("utf-8"))
    finally:
        os.close(fd)
    restrict_acl(token_file)
    return token


# ---------------------------------------------------------------------------
# 方法实现
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# 参数校验（服务入口是最容易被打畸形参数的地方，统一转成 400 而不是 500）
# ---------------------------------------------------------------------------
def _require_str(params: dict[str, Any], key: str) -> str:
    value = params.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ServiceError(f"参数 {key} 必须是非空字符串")
    return value


def _optional_str(params: dict[str, Any], key: str) -> Optional[str]:
    value = params.get(key)
    if value is None:
        return None
    if not isinstance(value, str) or not value.strip():
        raise ServiceError(f"参数 {key} 必须是非空字符串")
    return value


def _require_list(params: dict[str, Any], key: str) -> list[str]:
    value = params.get(key)
    if not isinstance(value, list) or not value:
        raise ServiceError(f"参数 {key} 必须是非空数组")
    return [str(v) for v in value]


def _as_int(
    params: dict[str, Any],
    key: str,
    default: Any = None,
    *,
    minimum: Optional[int] = None,
    maximum: Optional[int] = None,
) -> int:
    value = params.get(key, default)
    try:
        number = int(value)
    except (TypeError, ValueError) as exc:
        raise ServiceError(f"参数 {key} 必须是整数") from exc
    if minimum is not None and number < minimum:
        raise ServiceError(f"参数 {key} 不能小于 {minimum}")
    if maximum is not None and number > maximum:
        raise ServiceError(f"参数 {key} 不能大于 {maximum}")
    return number


def _as_flag(params: dict[str, Any], key: str, default: bool = False) -> bool:
    value = params.get(key, default)
    if isinstance(value, bool):
        return value
    raise ServiceError(f"参数 {key} 必须是布尔值")


def dispatch(session: Session, method: str, params: dict[str, Any]) -> Any:
    cfg = session.config
    if method == "ping":
        return {"name": "file_guard", "version": __version__, "home": str(cfg.home)}
    if method == "status":
        return {
            "home": str(cfg.home),
            "iterations": cfg.iterations,
            "chunk_size": cfg.chunk_size,
            "scan_workers": cfg.scan_workers,
            "backups": session.backups.stats(),
        }
    if method == "encrypt":
        results = []
        out = _optional_str(params, "out")
        force = _as_flag(params, "force")
        for path in _require_list(params, "paths"):
            info = encrypt_file(
                path,
                out,
                session.password,
                iterations=cfg.iterations,
                chunk_size=cfg.chunk_size,
                overwrite=force,
            )
            session.audit.record("encrypt", path=path, detail="服务端加密")
            results.append(info)
        return results
    if method == "decrypt":
        results = []
        out = _optional_str(params, "out")
        force = _as_flag(params, "force")
        for path in _require_list(params, "paths"):
            info = decrypt_file(
                path,
                out,
                session.password,
                overwrite=force,
            )
            session.audit.record("decrypt", path=path, detail="服务端解密")
            results.append(info)
        return results
    if method == "scan":
        min_risk = params.get("min_risk", "low")
        if min_risk not in ("high", "medium", "low"):
            raise ServiceError("参数 min_risk 只能是 high/medium/low")
        scanner = SensitiveScanner(
            max_file_size=_as_int(params, "max_file_size", cfg.scan_max_file_size, minimum=1, maximum=64 << 20),
            workers=_as_int(params, "workers", cfg.scan_workers, minimum=1, maximum=32),
            throttle_bps=_as_int(params, "throttle_bps", 0, minimum=0),
            skip_paths=[cfg.home],
        )
        report = scanner.scan(
            _require_list(params, "paths"),
            min_risk=str(min_risk),
        )
        session.audit.record(
            "scan",
            detail=f"扫描 {report.scanned} 个文件，高危 {report.summary.get('high', 0)} 个",
        )
        return report.to_dict()
    if method == "backup":
        results = session.backups.backup_paths(
            _require_list(params, "paths"), recursive=_as_flag(params, "recursive", True)
        )
        for item in results:
            if not item.get("skipped"):
                session.audit.record("backup", path=item.get("path"), detail=f"版本 {item.get('version')}")
        return results
    if method == "restore":
        path = _require_str(params, "path")
        version = params.get("version")
        if version is not None:
            version = _as_int(params, "version", minimum=1)
        result = session.backups.restore(
            path,
            version=version,
            dest=_optional_str(params, "dest"),
            overwrite=_as_flag(params, "force"),
        )
        session.audit.record("restore", path=path, detail=f"还原版本 {result.get('version')}")
        return result
    if method == "backup.list":
        return session.backups.list(_optional_str(params, "path"))
    if method == "backup.verify":
        return session.backups.verify(full=_as_flag(params, "full"))
    if method == "audit.list":
        return session.audit.query(
            event=_optional_str(params, "event"),
            contains=_optional_str(params, "contains"),
            limit=_as_int(params, "limit", 200, minimum=1, maximum=10_000),
        )
    if method == "audit.verify":
        return session.audit.verify()
    if method == "info":
        return inspect_container(_require_str(params, "path"))
    raise ServiceError(f"未知方法：{method}")


# ---------------------------------------------------------------------------
# HTTP 层
# ---------------------------------------------------------------------------
class _Handler(BaseHTTPRequestHandler):
    server_version = "FileGuard/1.0"
    sys_version = ""  # 不把 Python 版本号塞进 Server 响应头
    session: Session
    token: str
    strict_host: bool = True
    _slots: threading.BoundedSemaphore

    def log_message(self, fmt: str, *args: Any) -> None:  # 静默，避免刷屏
        pass

    def setup(self) -> None:
        super().setup()
        try:
            # 本机客户端只发一半请求就挂住时，别把线程永远占着
            self.connection.settimeout(REQUEST_TIMEOUT)
        except OSError:
            pass

    def _send(self, code: int, payload: dict[str, Any]) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        try:
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(body)
        except OSError:
            pass  # 对端已断开，不用管

    def _host_allowed(self) -> bool:
        """校验 Host 头：挡 DNS rebinding（恶意域名解析到 127.0.0.1 后借浏览器打本机端口）。"""
        if not self.strict_host:
            return True
        header = (self.headers.get("Host") or "").strip()
        if not header:
            return False
        if header.startswith("["):  # [::1]:8765
            end = header.find("]")
            hostname = header[1:end] if end > 0 else header
        elif header.count(":") == 1:  # 127.0.0.1:8765
            hostname = header.rsplit(":", 1)[0]
        else:
            hostname = header
        return is_loopback_host(hostname)

    def _origin_allowed(self) -> bool:
        """浏览器发起的请求一定带 Origin；本模块只面向原生进程调用，见到就拒绝。

        这挡的是"恶意网页让浏览器 POST 到 http://127.0.0.1:8765"这类 CSRF：
        即使拿不到响应，也可能触发加密/还原等有副作用的操作。
        """
        return not self.headers.get("Origin")

    def _reject_reason(self) -> Optional[tuple[int, str, str]]:
        """返回 (状态码, 错误类型, 提示) 或 None。"""
        if not self._host_allowed():
            return (403, "Forbidden", "Host 不被允许（只接受回环地址）")
        if not self._origin_allowed():
            return (403, "Forbidden", "不接受浏览器跨站请求（检测到 Origin）")
        return None

    def _drain_body(self) -> None:
        """拒绝请求前把请求体读掉。

        不读就直接关连接，Windows 会给客户端发 RST，客户端连拒绝原因都看不到
        （表现为 ConnectionAbortedError），排查体验很差。
        """
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if length <= 0:
            return
        if length > MAX_BODY:
            self.close_connection = True
            return
        try:
            self.rfile.read(length)
        except OSError:
            pass

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler 约定
        reason = self._reject_reason()
        if reason:
            self._send(reason[0], {"ok": False, "error": {"type": reason[1], "message": reason[2]}})
            return
        if self.path in ("/health", "/"):
            self._send(200, {"ok": True, "result": {"status": "alive", "version": __version__}})
        else:
            self._send(404, {"ok": False, "error": {"type": "NotFound", "message": "未知路径"}})

    def do_POST(self) -> None:  # noqa: N802
        reason = self._reject_reason()
        if reason:
            self._drain_body()
            self._send(reason[0], {"ok": False, "error": {"type": reason[1], "message": reason[2]}})
            return
        if self.path != "/rpc":
            self._drain_body()
            self._send(404, {"ok": False, "error": {"type": "NotFound", "message": "未知路径"}})
            return
        if not hmac.compare_digest(self.headers.get("X-FG-Token", ""), self.token):
            self._drain_body()
            self._send(401, {"ok": False, "error": {"type": "Unauthorized", "message": "token 无效"}})
            return
        if not self._slots.acquire(blocking=False):
            self._drain_body()
            self._send(429, {"ok": False, "error": {"type": "TooManyRequests", "message": "并发请求过多，请稍后重试"}})
            return
        try:
            self._handle_rpc()
        finally:
            self._slots.release()

    def _handle_rpc(self) -> None:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if length <= 0 or length > MAX_BODY:
            self._send(413, {"ok": False, "error": {"type": "BadRequest", "message": "请求体为空或过大"}})
            return
        try:
            raw = self.rfile.read(length)
        except OSError:
            return  # 连接超时/断开
        try:
            request = json.loads(raw.decode("utf-8"))
            method = str(request["method"])
            params = request.get("params") or {}
            if not isinstance(params, dict):
                raise ValueError("params 必须是对象")
        except (json.JSONDecodeError, KeyError, ValueError) as exc:
            self._send(400, {"ok": False, "error": {"type": "BadRequest", "message": str(exc)}})
            return
        try:
            result = dispatch(self.session, method, params)
            self._send(200, {"ok": True, "result": result})
        except FileGuardError as exc:
            code = 400 if getattr(exc, "exit_code", 1) == 1 else 409
            self._send(code, {"ok": False, "error": {"type": type(exc).__name__, "message": str(exc)}})
        except Exception as exc:  # noqa: BLE001 - 模块自身异常兜底，不能让服务进程挂掉
            self._send(500, {"ok": False, "error": {"type": "InternalError", "message": str(exc)}})


def create_server(
    session: Session,
    token: str,
    host: str = "127.0.0.1",
    port: int = 8765,
    *,
    allow_remote: bool = False,
) -> ThreadingHTTPServer:
    if not token or len(token) < SERVICE_TOKEN_MIN_LEN:
        raise ServiceError("服务 token 缺失或过短，拒绝启动")
    if not allow_remote and not is_loopback_host(host):
        raise ServiceError(
            f"只允许监听回环地址（当前 {host}）：本模块会代用户执行加解密，暴露到局域网风险很高；"
            "确认需要时请显式加 --allow-remote"
        )
    handler = type(
        "FileGuardHandler",
        (_Handler,),
        {
            "session": session,
            "token": token,
            "strict_host": not allow_remote,
            "_slots": threading.BoundedSemaphore(MAX_CONCURRENT_REQUESTS),
        },
    )
    return ThreadingHTTPServer((host, port), handler)


def serve(session: Session, *, host: str, port: int, token: str, allow_remote: bool = False) -> None:
    server = create_server(session, token, host, port, allow_remote=allow_remote)
    # flush=True 很重要：主程序用子进程拉起服务时要能立刻读到这行拿到实际端口，
    # 否则 stdout 是管道时会被块缓冲，读方会一直卡在 readline 上。
    print(
        json.dumps(
            {"event": "listening", "host": host, "port": server.server_port, "pid": os.getpid()},
            ensure_ascii=False,
        ),
        flush=True,
    )
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        session.close()
