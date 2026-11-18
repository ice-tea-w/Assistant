"""统一异常定义。

根据设计稿的"模块独立异常捕获"要求：所有异常都继承 FileGuardError，
主程序（桌面助手）只需要捕获这一个基类，模块内部任何失败都不会把异常
抛进主程序的事件循环。
"""

from __future__ import annotations


class FileGuardError(Exception):
    """文件安全模块的基类异常。"""

    exit_code = 1


class ConfigError(FileGuardError):
    """配置错误（home 目录、参数等）。"""


class PasswordError(FileGuardError):
    """密码问题：未提供、过短、校验失败。"""


class DecryptionError(FileGuardError):
    """解密失败：密码错误，或密文被篡改/损坏（GCM 校验不通过）。"""

    exit_code = 3


class CorruptContainerError(FileGuardError):
    """加密容器结构损坏：magic/版本/头部摘要/分块长度不正确。"""

    exit_code = 4


class IntegrityError(FileGuardError):
    """文件或模块完整性校验失败（哈希不匹配）。"""

    exit_code = 5


class ScanCancelledError(FileGuardError):
    """扫描被用户主动停止。"""


class ServiceError(FileGuardError):
    """本地服务相关错误（端口占用、token 无效等）。"""


class UnsupportedPlatformError(FileGuardError):
    """当前平台不支持该功能（例如非 Windows 上使用 DPAPI）。"""


#: Windows 常见错误码 → 可执行的提示（"拒绝访问"对用户没有信息量）
_WINERROR_HINTS = {
    5: "拒绝访问：文件可能被系统保护、目录只读，或需要更高权限",
    19: "磁盘写保护：请移除写保护后重试",
    32: "文件正被其他程序占用：请关闭占用它的程序（或稍等片刻）后重试",
    33: "文件被锁定：可能正在被其他程序独占使用",
    112: "磁盘空间不足：请清理空间或更换目标磁盘",
    123: "文件名或路径包含非法字符",
    206: "路径过长：请缩短路径，或在 Windows 中启用「长路径支持」",
    87: "参数或路径格式不正确",
    3: "路径不存在：请检查文件/目录是否已被移动或删除",
}


def describe_os_error(exc: OSError) -> str:
    """把 OSError 翻译成"用户能照着做"的提示。

    模块里大量操作最终都会落到文件系统（被占用、权限不足、路径过长、磁盘满），
    直接抛 ``[WinError 32] 另一个程序正在使用此文件`` 对桌面助手的用户毫无帮助。
    """
    winerror = getattr(exc, "winerror", None)
    if winerror in _WINERROR_HINTS:
        return f"{_WINERROR_HINTS[winerror]}（{exc}）"
    if isinstance(exc, PermissionError):
        return f"没有访问权限（{exc}）"
    if isinstance(exc, FileNotFoundError):
        return f"文件或目录不存在（{exc}）"
    if isinstance(exc, IsADirectoryError):
        return f"目标是一个目录，不是文件（{exc}）"
    if isinstance(exc, NotADirectoryError):
        return f"路径中有部分不是目录（{exc}）"
    if isinstance(exc, TimeoutError):
        return f"操作超时（{exc}）"
    return str(exc)


__all__ = [
    "ConfigError",
    "CorruptContainerError",
    "DecryptionError",
    "FileGuardError",
    "IntegrityError",
    "PasswordError",
    "ScanCancelledError",
    "ServiceError",
    "UnsupportedPlatformError",
    "describe_os_error",
]
