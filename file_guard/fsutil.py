"""文件系统层小工具：重解析点（符号链接/目录联结）检测、Windows ACL 收紧。

为什么单独抽出来：

* ``scan`` 必须只扫"用户明确指定的目录"，``watch`` 只能记录授权目录里的变化。
  但 ``os.walk`` 在 Windows 上会**跟着目录联结（junction）走**（``followlinks=False``
  只挡符号链接，挡不住 junction），于是 ``根目录/链接`` 可以把扫描/监听引到 ``C:\\Users``
  等未授权位置，违反模块最核心的"只碰授权路径"承诺。这里统一用
  ``is_reparse_point`` 把这类入口挡掉。
* 本地服务的 token 文件在 Windows 上必须显式收紧 ACL：``os.chmod(0o600)`` 在 Windows
  上只改只读属性，不改 ACL，同机其他用户仍能读到 token。
"""

from __future__ import annotations

import os
import stat
import subprocess
from pathlib import Path
from typing import Union

#: icacls 的"禁止窗口"标志，避免桌面助手（无控制台）里闪黑框
_CREATE_NO_WINDOW = 0x08000000


def is_reparse_point(path: Union[str, Path]) -> bool:
    """判断路径是否是符号链接 / 目录联结 / 挂载点（不跟随链接本身）。

    注意：故意**不**用 ``st_file_attributes & FILE_ATTRIBUTE_REPARSE_POINT`` 兜底判断，
    因为 OneDrive 等云盘占位文件也是重解析点，它们属于用户授权目录内的正常文件，
    跳过会漏扫。这里只挡"能把遍历带出授权目录"的链接类入口。
    """
    target = Path(path)
    try:
        st = os.lstat(target)
    except OSError:
        return False
    if stat.S_ISLNK(st.st_mode):
        return True
    if os.name != "nt":
        return False
    try:
        if os.path.islink(target):
            return True
    except OSError:
        pass
    isjunction = getattr(os.path, "isjunction", None)
    if isjunction is not None:
        try:
            if isjunction(target):
                return True
        except OSError:
            pass
    return False


def restrict_acl(path: Union[str, Path]) -> bool:
    """把文件/目录的访问权限收紧到"当前用户 + SYSTEM + Administrators"。

    返回是否成功。失败不抛异常（调用方决定是警告还是忽略）。
    """
    target = Path(path)
    if os.name != "nt":
        try:
            os.chmod(target, 0o600)
            return True
        except OSError:
            return False

    user = os.environ.get("USERNAME")
    if not user:
        return False
    domain = os.environ.get("USERDOMAIN")
    principal = f"{domain}\\{user}" if domain else user
    icacls = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "icacls.exe"
    if not icacls.exists():
        return False
    # 用 SID 而不是本地化名称，非英文 Windows 上也能用：
    #   *S-1-5-18 = SYSTEM，*S-1-5-32-544 = Administrators
    try:
        completed = subprocess.run(
            [
                str(icacls),
                str(target),
                "/inheritance:r",
                "/grant:r",
                f"{principal}:(F)",
                "*S-1-5-18:(F)",
                "*S-1-5-32-544:(F)",
            ],
            capture_output=True,
            timeout=30,
            creationflags=_CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        return completed.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


__all__ = ["is_reparse_point", "restrict_acl"]
