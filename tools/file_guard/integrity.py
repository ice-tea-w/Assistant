"""模块自身完整性校验（对应风险预案：恶意程序篡改安全模块）。

思路：
1. ``integrity init`` 记录包内每个 ``.py`` 文件的 SHA-256，生成清单；
2. 清单整体用密码派生的 HMAC 签名——攻击者改了代码但改不出合法签名；
3. ``integrity check`` 重新计算并比对，发现不一致即告警。

注意：这是"防误改/防低级篡改"的完整性自检，不是代码签名体系；
正式发版建议再用 Authenticode/代码签名证书对安装包签名。
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
from datetime import datetime
from pathlib import Path
from typing import Any, Union

from .crypto import MAX_ITERATIONS, MIN_ITERATIONS, Password, derive_key
from .errors import FileGuardError

MANIFEST_VERSION = 1
SIGN_ITERATIONS = 200_000
_HEX64_LEN = 64


def _validate_manifest(manifest: dict[str, Any]) -> None:
    """校验清单结构，避免"清单被改成畸形数据"直接抛 TypeError / 跑超大 KDF。

    清单本身是可被本地攻击者替换的文件：如果直接 ``int(manifest["iterations"])``
    再拿去派生密钥，一个 10 亿的迭代次数就能让校验卡死（DoS）；salt 长短不对则会
    抛底层异常。这里先在"不花算力"的前提下做结构校验。
    """
    if not isinstance(manifest, dict):
        raise FileGuardError("完整性清单格式不正确")
    if manifest.get("version") != MANIFEST_VERSION:
        raise FileGuardError(f"完整性清单版本不支持：{manifest.get('version')!r}")
    try:
        salt = base64.b64decode(str(manifest["salt"]), validate=True)
        iterations = int(manifest["iterations"])
    except (KeyError, ValueError, TypeError) as exc:
        raise FileGuardError("完整性清单字段缺失或格式非法") from exc
    if len(salt) < 16:
        raise FileGuardError("完整性清单 salt 长度异常")
    if not MIN_ITERATIONS <= iterations <= MAX_ITERATIONS:
        raise FileGuardError(f"完整性清单 KDF 迭代次数异常：{iterations}")
    files = manifest.get("files")
    if not isinstance(files, dict):
        raise FileGuardError("完整性清单 files 字段格式非法")
    for name, digest in files.items():
        if not isinstance(name, str) or not isinstance(digest, str) or len(digest) != _HEX64_LEN:
            raise FileGuardError("完整性清单文件条目格式非法")


def sha256_file(path: Union[str, Path]) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _module_files(root: Path) -> list[Path]:
    return sorted(
        p
        for p in root.rglob("*.py")
        if "__pycache__" not in p.parts and not any(part.startswith(".") for part in p.parts)
    )


def build_manifest(root: Union[str, Path], password: Password) -> dict[str, Any]:
    root = Path(root)
    salt = os.urandom(16)
    manifest: dict[str, Any] = {
        "version": MANIFEST_VERSION,
        "created": datetime.now().astimezone().isoformat(timespec="seconds"),
        "root": root.name,
        "salt": base64.b64encode(salt).decode(),
        "iterations": SIGN_ITERATIONS,
        "files": {p.relative_to(root).as_posix(): sha256_file(p) for p in _module_files(root)},
    }
    manifest["mac"] = _sign(manifest, password)
    return manifest


def _canonical(manifest: dict[str, Any]) -> bytes:
    body = {k: v for k, v in manifest.items() if k != "mac"}
    return json.dumps(body, sort_keys=True, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def _sign(manifest: dict[str, Any], password: Password) -> str:
    key = derive_key(password, base64.b64decode(manifest["salt"]), int(manifest["iterations"]))
    return hmac.new(key, _canonical(manifest), hashlib.sha256).hexdigest()


def verify_manifest(
    root: Union[str, Path], manifest: dict[str, Any], password: Password
) -> dict[str, Any]:
    root = Path(root)
    problems: list[str] = []
    try:
        _validate_manifest(manifest)
    except FileGuardError as exc:
        return {
            "ok": False,
            "checked": 0,
            "modified": [],
            "missing": [],
            "extra": [],
            "problems": [str(exc)],
        }
    if not hmac.compare_digest(_sign(manifest, password), str(manifest.get("mac", ""))):
        problems.append("清单签名无效（清单被修改，或密码不正确）")
    expected: dict[str, str] = dict(manifest.get("files", {}))
    actual = {p.relative_to(root).as_posix(): sha256_file(p) for p in _module_files(root)}
    modified = [name for name, digest in expected.items() if name in actual and actual[name] != digest]
    missing = [name for name in expected if name not in actual]
    extra = [name for name in actual if name not in expected]
    if modified:
        problems.append(f"被修改：{', '.join(sorted(modified)[:10])}")
    if missing:
        problems.append(f"缺失：{', '.join(sorted(missing)[:10])}")
    if extra:
        problems.append(f"新增未签名文件：{', '.join(sorted(extra)[:10])}")
    return {
        "ok": not problems,
        "checked": len(actual),
        "modified": sorted(modified),
        "missing": sorted(missing),
        "extra": sorted(extra),
        "problems": problems,
    }


def save_manifest(manifest: dict[str, Any], path: Union[str, Path]) -> Path:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(tmp, path)
    return path


def load_manifest(path: Union[str, Path]) -> dict[str, Any]:
    path = Path(path)
    if not path.exists():
        raise FileGuardError(f"完整性清单不存在：{path}（先执行 integrity init）")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise FileGuardError(f"完整性清单损坏：{path}") from exc
