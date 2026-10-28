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
