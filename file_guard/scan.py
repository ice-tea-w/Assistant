"""敏感文件风险检测（业务层：检测）。

对应设计稿"敏感文件风险检测"：

* 完全由用户主动触发，不做后台全盘静默扫描（``scan()`` 只接受用户给出的目录）；
* 识别身份证号、手机号、银行卡号、合同/协议、涉密标记、密码/密钥关键词等；
* 风险分级 高/中/低，给出分值，供 UI 弹窗排序；
* 扫描支持暂停/继续/停止、限速、多线程，默认低 CPU 占用；
* 只读不写：扫描过程不修改任何用户文件。

实现上只依赖标准库（zipfile 可读 docx/xlsx/pptx 的 XML 文本），
PDF 等格式留给后续接 pypdf（见 docs 中的扩展点）。
"""

from __future__ import annotations

import os
import re
import threading
import time
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import asdict, dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Callable, Iterable, Optional, Sequence, Union

from .errors import FileGuardError, ScanCancelledError
from .fsutil import is_reparse_point

SEVERITY_ORDER = {"high": 3, "medium": 2, "low": 1, "none": 0}
SEVERITY_LABEL = {"high": "高", "medium": "中", "low": "低", "none": "无"}
SEVERITY_WEIGHT = {"high": 30, "medium": 10, "low": 3}

#: 文本类扩展名（直接按文本读取）
TEXT_EXTENSIONS = {
    ".txt", ".md", ".markdown", ".csv", ".tsv", ".json", ".jsonl", ".log", ".xml",
    ".yml", ".yaml", ".ini", ".conf", ".cfg", ".env", ".py", ".js", ".ts", ".java",
    ".c", ".cpp", ".h", ".hpp", ".cs", ".go", ".rs", ".php", ".rb", ".sql", ".sh",
    ".ps1", ".bat", ".cmd", ".html", ".htm", ".css", ".rtf",
}
#: Office 文档（zip + XML，标准库即可抽取文本）
OFFICE_EXTENSIONS = {".docx", ".xlsx", ".pptx", ".docm", ".xlsm", ".pptm"}
#: 其他常见文档：无法直接读内容，只按文件名做风险提示
OPAQUE_EXTENSIONS = {".pdf", ".doc", ".xls", ".ppt", ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tif", ".tiff"}

DEFAULT_SKIP_DIR_NAMES = {
    ".git", ".svn", ".hg", "__pycache__", "node_modules", "$recycle.bin",
    "system volume information", ".venv", "venv",
}

MAX_FINDINGS_PER_FILE = 20
#: 单个 Office XML 分部的解压后上限（防 zip 炸弹）
MAX_OFFICE_ENTRY = 4 << 20
#: 一个 Office 文件所有 XML 分部的累计解压上限
MAX_OFFICE_TOTAL = 8 << 20


# ---------------------------------------------------------------------------
# 标识符校验（降低误报，体现"检测"而非"猜"）
# ---------------------------------------------------------------------------
ID_WEIGHTS = (7, 9, 10, 5, 8, 4, 2, 1, 6, 3, 7, 9, 10, 5, 8, 4, 2)
ID_CHECK_CODES = "10X98765432"


def is_valid_id_card(value: str) -> bool:
    if len(value) != 18:
        return False
    if not value[:17].isdigit():
        return False
    total = sum(int(c) * w for c, w in zip(value[:17], ID_WEIGHTS))
    return ID_CHECK_CODES[total % 11] == value[17].upper()


def is_valid_luhn(value: str) -> bool:
    digits = [int(c) for c in value if c.isdigit()]
    if len(digits) < 12:
        return False
    checksum = 0
    for i, digit in enumerate(reversed(digits)):
        if i % 2 == 1:
            digit *= 2
            if digit > 9:
                digit -= 9
        checksum += digit
    return checksum % 10 == 0


# ---------------------------------------------------------------------------
# 检测规则
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class Rule:
    kind: str
    label: str
    severity: str
    pattern: re.Pattern[str]
    validator: Optional[Callable[[str], bool]] = None


RULES: tuple[Rule, ...] = (
    Rule(
        "id_card",
        "身份证号",
        "high",
        re.compile(r"(?<!\d)([1-9]\d{5}(?:18|19|20)\d{2}(?:0[1-9]|1[0-2])(?:0[1-9]|[12]\d|3[01])\d{3}[\dXx])(?!\d)"),
        is_valid_id_card,
    ),
    Rule("bank_card", "银行卡号", "high", re.compile(r"(?<!\d)(\d{16,19})(?!\d)"), is_valid_luhn),
    Rule("phone", "手机号", "medium", re.compile(r"(?<!\d)(1[3-9]\d{9})(?!\d)")),
    Rule("email", "邮箱地址", "low", re.compile(r"[\w.+-]+@[\w-]+\.[\w.-]+")),
    Rule(
        "credentials",
        "密码/密钥信息",
        "high",
        re.compile(
            r"(?i)(?:密码|口令|password|passwd|pwd|secret[-_ ]?key|api[-_ ]?key|access[-_ ]?key|token)\s*[:=：]\s*\S+"
        ),
    ),
    Rule(
        "classified",
        "涉密/内部标记",
        "medium",
        re.compile(r"(绝密|机密|秘密文件|内部资料|仅限内部|禁止外传|confidential)"),
    ),
    Rule(
        "contract",
        "合同/协议",
        "low",
        re.compile(r"(劳动合同|保密协议|采购合同|销售合同|租赁合同|合作协议|甲方[：:]|乙方[：:]|报价单|付款条款)"),
    ),
    Rule(
        "privacy_table",
        "隐私表格（姓名+证件/手机）",
        "medium",
        re.compile(r"姓名.{0,20}(身份证|证件号|手机号|电话).{0,20}"),
    ),
)

#: 文件名中的敏感关键词（对 PDF/图片等无法读正文的文件也生效）
NAME_RULES: tuple[tuple[str, str, str], ...] = (
    ("id_card", "身份证", "high"),
    ("bank_card", "银行卡", "high"),
    ("credential", "密码", "high"),
    ("contract", "合同", "medium"),
    ("resume", "简历", "medium"),
    ("finance", "工资", "medium"),
    ("privacy", "隐私", "medium"),
)


def mask_value(value: str) -> str:
    """日志/报告中只展示打码后的样本，避免二次泄露。"""
    value = value.strip()
    if len(value) <= 4:
        return "*" * len(value)
    keep = 2 if len(value) <= 10 else 3
    return value[:keep] + "*" * (len(value) - keep * 2) + value[-keep:]


# ---------------------------------------------------------------------------
# 扫描控制（暂停/继续/停止）
# ---------------------------------------------------------------------------
class ScanControl:
    """协作式扫描控制：工作线程每处理一个文件前调用 ``checkpoint()``。"""

    def __init__(self) -> None:
        self._stop = threading.Event()
        self._resume = threading.Event()
        self._resume.set()

    @property
    def stopped(self) -> bool:
        return self._stop.is_set()

    @property
    def paused(self) -> bool:
        return not self._resume.is_set()

    def stop(self) -> None:
        self._stop.set()
        self._resume.set()  # 唤醒暂停中的线程，让它们尽快退出

    def pause(self) -> None:
        if not self._stop.is_set():
            self._resume.clear()

    def resume(self) -> None:
        self._resume.set()

    def checkpoint(self) -> None:
        if self._stop.is_set():
            raise ScanCancelledError("扫描已停止")
        if not self._resume.is_set():
            while not self._resume.wait(0.1):
                if self._stop.is_set():
                    raise ScanCancelledError("扫描已停止")


# ---------------------------------------------------------------------------
# 结果模型
# ---------------------------------------------------------------------------
@dataclass
class Finding:
    kind: str
    label: str
    severity: str
    line: int
    sample: str  # 已打码

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass
class FileRisk:
    path: str
    size: int
    risk: str = "none"
    score: int = 0
    findings: list[Finding] = field(default_factory=list)
    note: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {
            "path": self.path,
            "size": self.size,
            "risk": self.risk,
            "risk_label": SEVERITY_LABEL[self.risk],
            "score": self.score,
            "findings": [f.to_dict() for f in self.findings],
            "note": self.note,
        }


@dataclass
class ScanReport:
    roots: list[str] = field(default_factory=list)
    started: str = ""
    finished: str = ""
    scanned: int = 0
    skipped: int = 0
    cancelled: bool = False
    files: list[FileRisk] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)
    summary: dict[str, int] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {
            "roots": self.roots,
            "started": self.started,
            "finished": self.finished,
            "scanned": self.scanned,
            "skipped": self.skipped,
            "cancelled": self.cancelled,
            "summary": self.summary,
            "files": [f.to_dict() for f in self.files],
            "errors": self.errors,
        }


# ---------------------------------------------------------------------------
# 扫描器
# ---------------------------------------------------------------------------
class SensitiveScanner:
    def __init__(
        self,
        *,
        max_file_size: int = 2 << 20,
        workers: int = 4,
        throttle_bps: int = 0,
        skip_dir_names: Optional[set[str]] = None,
        skip_paths: Optional[Sequence[Union[str, Path]]] = None,
        progress: Optional[Callable[[int, int, Optional[str]], None]] = None,
    ) -> None:
        self.max_file_size = max_file_size
        self.workers = max(1, workers)
        self.throttle_bps = max(0, throttle_bps)
        self.skip_dir_names = {n.lower() for n in (skip_dir_names or DEFAULT_SKIP_DIR_NAMES)}
        self.skip_paths = [Path(p).resolve() for p in (skip_paths or [])]
        self.progress = progress
        self._throttle_lock = threading.Lock()
        self._throttle_allowance = 0.0
        self._throttle_ts = time.monotonic()
        self._system_dirs = self._detect_system_dirs()

    # ---- 路径过滤 -------------------------------------------------
    @staticmethod
    def _detect_system_dirs() -> list[Path]:
        candidates: list[Path] = []
        for var in ("SystemRoot", "ProgramFiles", "ProgramFiles(x86)", "ProgramData"):
            value = os.environ.get(var)
            if value:
                try:
                    candidates.append(Path(value).resolve())
                except OSError:
                    continue
        return candidates

    def _should_skip(self, path: Path) -> Optional[str]:
        try:
            resolved = path.resolve()
        except OSError:
            return "无法解析路径"
        for skip in self.skip_paths:
            if resolved == skip or skip in resolved.parents:
                return "位于排除目录"
        for system in self._system_dirs:
            if resolved == system or system in resolved.parents:
                return "系统目录（按设计自动跳过）"
        if any(part.lower() in self.skip_dir_names for part in resolved.parts):
            return "命中跳过目录名"
        if resolved.name.endswith(".fgc"):
            return "已是加密容器"
        return None

    def collect(self, roots: Iterable[Union[str, Path]], control: Optional[ScanControl] = None) -> list[Path]:
        """收集待扫描文件。

        只收集**明确授权根目录下**的普通文件：目录联结/符号链接一律跳过，否则
        ``根目录/链接 -> C:\\Users`` 会把扫描带出授权范围（隐私红线）。
        用户自己把链接当作根传进来时视为显式授权，不做拦截。
        """
        files: list[Path] = []
        for root in roots:
            root = Path(root).expanduser()
            if control:
                control.checkpoint()
            if root.is_file():
                files.append(root)
                continue
            if not root.is_dir():
                raise FileGuardError(f"扫描路径不存在：{root}")
            for dirpath, dirnames, filenames in os.walk(root):
                if control and control.stopped:
                    break
                current = Path(dirpath)
                dirnames[:] = [
                    d
                    for d in dirnames
                    if d.lower() not in self.skip_dir_names and not is_reparse_point(current / d)
                ]
                for name in filenames:
                    path = current / name
                    if path.name.endswith(".fgc"):
                        continue  # 已是加密容器，后面也会跳过，这里省一次 resolve
                    if is_reparse_point(path):
                        continue  # 文件符号链接：目标可能在授权目录之外
                    files.append(path)
        return files

    # ---- 限速 -----------------------------------------------------
    def _throttle(self, nbytes: int) -> None:
        if self.throttle_bps <= 0:
            return
        with self._throttle_lock:
            now = time.monotonic()
            self._throttle_allowance += (now - self._throttle_ts) * self.throttle_bps
            self._throttle_ts = now
            self._throttle_allowance -= nbytes
            if self._throttle_allowance < 0:
                sleep = -self._throttle_allowance / self.throttle_bps
                time.sleep(min(sleep, 1.0))
                self._throttle_allowance = 0.0

    # ---- 单文件扫描 -----------------------------------------------
    def scan_file(self, path: Union[str, Path]) -> FileRisk:
        path = Path(path)
        try:
            stat = path.stat()
        except OSError as exc:
            return FileRisk(path=str(path), size=0, note=f"无法读取：{exc}")
        risk = FileRisk(path=str(path), size=stat.st_size)
        if stat.st_size > self.max_file_size:
            risk.note = f"文件超过 {self.max_file_size} 字节，跳过正文检测"
            risk.findings.extend(self._name_findings(path))
            return self._finalize(risk)
        suffix = path.suffix.lower()
        text = ""
        if suffix in OFFICE_EXTENSIONS:
            text = self._read_office_text(path)
        elif suffix in TEXT_EXTENSIONS or suffix == "":
            text = self._read_text(path)
        else:
            risk.note = "非文本格式，仅检测文件名"
        if text:
            risk.findings.extend(self._content_findings(text))
        risk.findings.extend(self._name_findings(path))
        return self._finalize(risk)

    def _read_text(self, path: Path) -> str:
        try:
            # 只读上限字节数：先 stat 再 read_bytes() 会有 TOCTOU，文件在两步之间变大
            # 时仍然会把整个文件读进内存
            with open(path, "rb") as f:
                data = f.read(self.max_file_size + 1)
        except OSError:
            return ""
        if b"\x00" in data[:8192]:
            return ""  # 二进制
        return self._decode(data)

    @staticmethod
    def _decode(data: bytes) -> str:
        for encoding in ("utf-8", "gb18030"):
            try:
                return data.decode(encoding)
            except UnicodeDecodeError:
                continue
        return data.decode("utf-8", errors="ignore")

    @staticmethod
    def _read_office_text(path: Path) -> str:
        """从 docx/xlsx/pptx 的 zip 包中抽取 XML 文本（纯标准库）。

        防 zip 炸弹：**先看声明的解压后大小**再决定读不读；读取本身也用
        ``ZipExtFile.read(n)`` 限制上限。旧实现是 ``zf.read(name)`` 完整解压
        之后才检查大小——一个 10 KB 的 docx 就能让进程吃掉几 GB 内存。
        """
        chunks: list[str] = []
        total = 0
        try:
            with zipfile.ZipFile(path) as zf:
                names = [
                    n
                    for n in zf.namelist()
                    if n.endswith(".xml")
                    and (
                        n.startswith("word/")
                        or n.startswith("xl/")
                        or n.startswith("ppt/slides/")
                        or n == "xl/sharedStrings.xml"
                    )
                ]
                for name in names[:50]:
                    try:
                        declared = zf.getinfo(name).file_size
                    except KeyError:
                        continue
                    if declared > MAX_OFFICE_ENTRY:
                        continue
                    if total + declared > MAX_OFFICE_TOTAL:
                        break
                    with zf.open(name) as handle:
                        # 声明大小可能造假：真正读取时再兜一次上限
                        raw = handle.read(MAX_OFFICE_ENTRY + 1)
                    if len(raw) > MAX_OFFICE_ENTRY:
                        continue
                    total += len(raw)
                    chunks.append(re.sub(rb"<[^>]+>", b" ", raw).decode("utf-8", errors="ignore"))
        except (zipfile.BadZipFile, OSError, KeyError, RuntimeError):
            return ""
        return "\n".join(chunks)

    def _content_findings(self, text: str) -> list[Finding]:
        findings: list[Finding] = []
        seen: set[tuple[str, int]] = set()
        for lineno, line in enumerate(text.splitlines(), 1):
            if len(findings) >= MAX_FINDINGS_PER_FILE:
                break
            for rule in RULES:
                if len(findings) >= MAX_FINDINGS_PER_FILE:
                    break
                for match in rule.pattern.finditer(line):
                    value = match.group(0)
                    if rule.validator and not rule.validator(value):
                        continue
                    key = (rule.kind, lineno)
                    if key in seen:
                        continue
                    seen.add(key)
                    findings.append(
                        Finding(
                            kind=rule.kind,
                            label=rule.label,
                            severity=rule.severity,
                            line=lineno,
                            sample=mask_value(value),
                        )
                    )
                    break  # 同一行同一规则只记一次
        return findings

    @staticmethod
    def _name_findings(path: Path) -> list[Finding]:
        name = path.stem.lower()
        findings: list[Finding] = []
        for kind, keyword, severity in NAME_RULES:
            if keyword in name:
                findings.append(
                    Finding(kind=kind, label=f"文件名含“{keyword}”", severity=severity, line=0, sample=path.name)
                )
        return findings

    @staticmethod
    def _finalize(risk: FileRisk) -> FileRisk:
        if not risk.findings:
            risk.risk = "none"
            risk.score = 0
            return risk
        risk.risk = max((f.severity for f in risk.findings), key=lambda s: SEVERITY_ORDER[s])
        score = sum(SEVERITY_WEIGHT[f.severity] for f in risk.findings)
        risk.score = min(100, score + min(len(risk.findings), 5) * 2)
        risk.findings.sort(key=lambda f: SEVERITY_ORDER[f.severity], reverse=True)
        return risk

    # ---- 目录/文件批量扫描 ----------------------------------------
    def scan(
        self,
        roots: Iterable[Union[str, Path]],
        *,
        control: Optional[ScanControl] = None,
        min_risk: str = "low",
        max_files: int = 0,
    ) -> ScanReport:
        control = control or ScanControl()
        roots = [Path(r).expanduser() for r in roots]
        report = ScanReport(roots=[str(r) for r in roots], started=datetime.now().isoformat(timespec="seconds"))
        try:
            paths = self.collect(roots, control)
        except ScanCancelledError:
            report.cancelled = True
            report.finished = datetime.now().isoformat(timespec="seconds")
            report.summary = {"high": 0, "medium": 0, "low": 0, "none": 0, "findings": 0, "files": 0}
            return report
        if max_files > 0:
            paths = paths[:max_files]
        total = len(paths)
        report.skipped = 0
        results: list[FileRisk] = []

        def work(p: Path) -> Optional[FileRisk]:
            control.checkpoint()
            skip_reason = self._should_skip(p)
            if skip_reason:
                return None
            risk = self.scan_file(p)
            self._throttle(risk.size)
            return risk

        cancelled = False
        with ThreadPoolExecutor(max_workers=self.workers) as pool:
            futures = {pool.submit(work, p): p for p in paths}
            try:
                for done, future in enumerate(as_completed(futures), 1):
                    try:
                        risk = future.result()
                    except ScanCancelledError:
                        cancelled = True
                        for f in futures:
                            f.cancel()
                        break
                    except Exception as exc:  # 单个文件失败不影响整体扫描
                        report.errors.append(f"{futures[future]}：{exc}")
                        continue
                    if risk is None:
                        report.skipped += 1
                        continue
                    report.scanned += 1
                    results.append(risk)
                    if self.progress:
                        self.progress(done, total, str(futures[future]))
            except KeyboardInterrupt:
                control.stop()
                cancelled = True
                for f in futures:
                    f.cancel()

        report.cancelled = cancelled or control.stopped
        report.finished = datetime.now().isoformat(timespec="seconds")
        threshold = SEVERITY_ORDER.get(min_risk, 1)
        report.files = sorted(
            (r for r in results if SEVERITY_ORDER[r.risk] >= threshold),
            key=lambda r: (-r.score, r.path),
        )
        report.summary = {
            "high": sum(1 for r in results if r.risk == "high"),
            "medium": sum(1 for r in results if r.risk == "medium"),
            "low": sum(1 for r in results if r.risk == "low"),
            "none": sum(1 for r in results if r.risk == "none"),
            "findings": sum(len(r.findings) for r in results),
            "files": report.scanned,
        }
        return report


def scan_paths(
    paths: Iterable[Union[str, Path]],
    *,
    max_file_size: int = 2 << 20,
    workers: int = 4,
    min_risk: str = "low",
    throttle_bps: int = 0,
    control: Optional[ScanControl] = None,
    progress: Optional[Callable[[int, int, Optional[str]], None]] = None,
    skip_paths: Sequence[Union[str, Path]] = (),
) -> ScanReport:
    """便捷入口：给路径列表，返回扫描报告。"""
    scanner = SensitiveScanner(
        max_file_size=max_file_size,
        workers=workers,
        throttle_bps=throttle_bps,
        progress=progress,
        skip_paths=skip_paths,
    )
    return scanner.scan(paths, control=control, min_risk=min_risk)
