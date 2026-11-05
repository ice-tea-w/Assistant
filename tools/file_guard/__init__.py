"""file_guard —— 桌面助手「文件安全模块」参考实现。

四大子模块（对应设计方案第 5~7 页）：

* :mod:`file_guard.crypto`   文件加密解密（AES-256-GCM / PBKDF2 / 密钥文件 / DPAPI 保险箱）
* :mod:`file_guard.scan`     敏感文件风险检测（主动扫描、分级、暂停限速）
* :mod:`file_guard.audit`    操作审计日志（逐条加密 + HMAC 哈希链）
* :mod:`file_guard.backup`   安全备份与恢复（增量、加密、版本回滚）

外层：:mod:`file_guard.cli`（命令行/右键菜单）、:mod:`file_guard.service`（本地 HTTP 服务）、
:mod:`file_guard.watch`（目录监听）、:mod:`file_guard.integrity`（模块自校验）、
:mod:`file_guard.fsutil`（重解析点检测 / ACL 收紧）。
"""

from __future__ import annotations

__version__ = "0.2.0"

from .audit import AuditLog, EncryptedLog, LogRecord  # noqa: E402
from .backup import BackupStore  # noqa: E402
from .config import Config  # noqa: E402
from .crypto import (  # noqa: E402
    DpapiVault,
    decrypt_file,
    encrypt_file,
    generate_keyfile,
    inspect_container,
    is_container,
    load_keyfile_material,
    rekey_file,
    verify_file,
)
from .errors import (  # noqa: E402
    CorruptContainerError,
    DecryptionError,
    FileGuardError,
    IntegrityError,
    PasswordError,
    ScanCancelledError,
)
from .scan import (  # noqa: E402
    FileRisk,
    Finding,
    ScanControl,
    ScanReport,
    SensitiveScanner,
    scan_paths,
)

__all__ = [
    "AuditLog",
    "BackupStore",
    "Config",
    "CorruptContainerError",
    "DecryptionError",
    "DpapiVault",
    "EncryptedLog",
    "FileGuardError",
    "FileRisk",
    "Finding",
    "IntegrityError",
    "LogRecord",
    "PasswordError",
    "ScanCancelledError",
    "ScanControl",
    "ScanReport",
    "SensitiveScanner",
    "__version__",
    "decrypt_file",
    "encrypt_file",
    "generate_keyfile",
    "inspect_container",
    "is_container",
    "load_keyfile_material",
    "rekey_file",
    "scan_paths",
    "verify_file",
]
