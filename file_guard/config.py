"""运行配置与目录布局。

设计稿要求"本地优先、最小权限"：所有数据默认落在
``%USERPROFILE%\\.file_guard``（可通过 FG_HOME 或 --home 覆盖），
程序本身不联网、不上传。

配置文件的信任边界：``config.json`` 是普通 JSON，可能被手改/被同机程序改坏。
加载时逐字段校验类型和范围（而不是 ``setattr`` 一把梭），既能避免 TypeError
把主程序带崩，也能挡住"把 iterations 改成 1"这种把安全强度归零的误操作。
"""

from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field, fields
from pathlib import Path
from typing import Any

from .errors import ConfigError

APP_NAME = "file_guard"
ENV_HOME = "FG_HOME"
ENV_PASSWORD = "FG_PASSWORD"
ENV_ITERATIONS = "FG_KDF_ITERATIONS"

MIN_ITERATIONS = 1_000
MAX_ITERATIONS = 5_000_000
MIN_CHUNK_SIZE = 256
MAX_CHUNK_SIZE = 64 << 20
MAX_SCAN_FILE_SIZE = 64 << 20


def default_home() -> Path:
    env = os.environ.get(ENV_HOME)
    if env:
        return Path(env).expanduser()
    return Path.home() / ".file_guard"


@dataclass
class Config:
    """全局配置。"""

    home: Path = field(default_factory=default_home)
    #: PBKDF2 迭代次数（设计稿：加盐哈希、防暴力破解）
    iterations: int = 600_000
    #: 容器分块大小（AES-GCM 每块独立认证，避免整文件读入内存）
    chunk_size: int = 1 << 20
    #: 审计日志/备份索引的 KDF 迭代次数（日志条目小，取 1/3 即可）
    log_iterations: int = 200_000
    #: 扫描默认参数
    scan_workers: int = max(2, min(8, (os.cpu_count() or 4)))
    scan_max_file_size: int = 2 << 20
    #: 本地服务监听地址（只绑 127.0.0.1，天然不允许远程访问）
    service_host: str = "127.0.0.1"
    service_port: int = 8765
    #: 备份保留的版本数（0 表示不限制）
    backup_keep: int = 20

    # ---- 目录布局 -------------------------------------------------
    @property
    def config_file(self) -> Path:
        return self.home / "config.json"

    @property
    def vault_file(self) -> Path:
        return self.home / "vault.bin"  # DPAPI 加密的密码缓存

    @property
    def audit_dir(self) -> Path:
        return self.home / "audit"

    @property
    def backup_dir(self) -> Path:
        return self.home / "backups"

    @property
    def token_file(self) -> Path:
        return self.home / "service.token"

    @property
    def integrity_file(self) -> Path:
        return self.home / "integrity.json"

    # ---- 校验 -----------------------------------------------------
    def validate(self) -> None:
        """校验各字段类型与范围；非法即报 ConfigError（不静默纠正）。"""
        if not isinstance(self.home, Path) or not str(self.home).strip():
            raise ConfigError("配置项 home 非法")
        checks = (
            ("iterations", self.iterations, MIN_ITERATIONS, MAX_ITERATIONS),
            ("log_iterations", self.log_iterations, MIN_ITERATIONS, MAX_ITERATIONS),
            ("chunk_size", self.chunk_size, MIN_CHUNK_SIZE, MAX_CHUNK_SIZE),
            ("scan_workers", self.scan_workers, 1, 64),
            ("scan_max_file_size", self.scan_max_file_size, 1, MAX_SCAN_FILE_SIZE),
            ("service_port", self.service_port, 1, 65535),
            ("backup_keep", self.backup_keep, 0, 100_000),
        )
        for name, value, minimum, maximum in checks:
            if isinstance(value, bool) or not isinstance(value, int):
                raise ConfigError(f"配置项 {name} 必须是整数，实际是 {type(value).__name__}")
            if not minimum <= value <= maximum:
                raise ConfigError(f"配置项 {name}={value} 超出允许范围 [{minimum}, {maximum}]")
        if not isinstance(self.service_host, str) or not self.service_host.strip():
            raise ConfigError("配置项 service_host 非法")

    # ---- 读写 -----------------------------------------------------
    def ensure_dirs(self) -> None:
        self.home.mkdir(parents=True, exist_ok=True)
        self.audit_dir.mkdir(parents=True, exist_ok=True)

    def save(self) -> None:
        self.validate()
        self.ensure_dirs()
        data = asdict(self)
        data["home"] = str(self.home)
        tmp = self.config_file.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(tmp, self.config_file)

    @classmethod
    def load(cls, home: Path | str | None = None) -> "Config":
        home = Path(home).expanduser() if home else default_home()
        cfg = cls(home=home)
        if cfg.config_file.exists():
            try:
                raw: dict[str, Any] = json.loads(cfg.config_file.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as exc:  # 配置损坏不应让主程序崩溃
                raise ConfigError(f"配置文件损坏：{cfg.config_file}（{exc}）") from exc
            if not isinstance(raw, dict):
                raise ConfigError(f"配置文件格式不正确：{cfg.config_file}")
            allowed = {item.name for item in fields(cls)} - {"home"}
            for key, value in raw.items():
                if key not in allowed:
                    continue  # 未知字段忽略（向前兼容），但绝不 setattr 到属性上
                setattr(cfg, key, value)
        cfg.apply_env()
        cfg.validate()
        return cfg

    def apply_env(self) -> None:
        """测试/批量场景可用环境变量降低 KDF 成本。"""
        if os.environ.get(ENV_ITERATIONS):
            try:
                it = int(os.environ[ENV_ITERATIONS])
            except ValueError as exc:
                raise ConfigError(f"{ENV_ITERATIONS} 不是整数") from exc
            self.iterations = it
            self.log_iterations = max(MIN_ITERATIONS, it // 3)
