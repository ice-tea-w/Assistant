"""端到端演示：加密 → 检测 → 审计 → 备份恢复，全程本地执行、不联网。

运行（需要先 pip install cryptography）::

    python examples/demo.py

演示数据全部是随机生成的假数据，运行在系统临时目录，结束自动清理。
"""

from __future__ import annotations

import shutil
import sys
import tempfile
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from file_guard.audit import AuditLog  # noqa: E402
from file_guard.backup import BackupStore  # noqa: E402
from file_guard.config import Config  # noqa: E402
from file_guard.crypto import decrypt_file, encrypt_file  # noqa: E402
from file_guard.errors import DecryptionError  # noqa: E402
from file_guard.scan import ScanControl, SensitiveScanner  # noqa: E402

PASSWORD = "demo-password-123"
ITERATIONS = 2_000  # 演示用低成本 KDF；生产默认 600000
CHUNK_SIZE = 4096


def fake_id_card() -> str:
    body = "11010519900101123"
    weights = (7, 9, 10, 5, 8, 4, 2, 1, 6, 3, 7, 9, 10, 5, 8, 4, 2)
    total = sum(int(c) * w for c, w in zip(body, weights))
    return body + "10X98765432"[total % 11]


def fake_bank_card() -> str:
    prefix = "622202123456789"
    total = 0
    for i, digit in enumerate(reversed(prefix)):
        if i % 2 == 0:
            digit = int(digit) * 2
            if digit > 9:
                digit -= 9
        total += int(digit)
    return prefix + str((10 - total % 10) % 10)


def line(title: str) -> None:
    print(f"\n{'=' * 12} {title} {'=' * 12}")


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="file-guard-demo-"))
    try:
        work = tmp / "work"
        work.mkdir()
        home = tmp / "home"

        # ---- 造一批演示文件 -------------------------------------
        (work / "客户名单.csv").write_text(
            "姓名,身份证号,手机号,银行卡\n"
            f"张三,{fake_id_card()},13812345678,{fake_bank_card()}\n"
            "李四,11010519920304123X,13900001111,6222020000000002\n",
            encoding="utf-8",
        )
        (work / "会议纪要.txt").write_text("本周例会：讨论桌面助手下一阶段排期。\n", encoding="utf-8")
        (work / "2026年劳动合同.pdf").write_bytes(b"%PDF-1.4 fake binary content")
        (work / "config.ini").write_text("password = SuperSecret123\n", encoding="utf-8")
        print(f"演示目录：{work}")

        # ---- 1. 加密 / 解密 -------------------------------------
        line("1. 文件加密解密（AES-256-GCM + PBKDF2）")
        cfg = Config(home=home, iterations=ITERATIONS, chunk_size=CHUNK_SIZE)
        cfg.ensure_dirs()
        cfg.save()
        src = work / "客户名单.csv"
        info = encrypt_file(src, password=PASSWORD, iterations=ITERATIONS, chunk_size=CHUNK_SIZE)
        print(f"[OK] 生成加密容器：{info['dst']}")
        print(f"     容器大小 {info['ciphertext_size']} B，明文 {info['plaintext_size']} B，{info['chunks']} 个分块")
        print(f"     明文 SHA-256：{info['sha256'][:32]}...")

        restored = tmp / "restored.csv"
        decrypt_file(info["dst"], restored, PASSWORD)
        assert restored.read_bytes() == src.read_bytes()
        print("[OK] 解密后逐字节比对一致")

        # 篡改演示：改一个字节，GCM 立刻拒绝
        tampered = tmp / "tampered.fgc"
        shutil.copyfile(info["dst"], tampered)
        raw = bytearray(tampered.read_bytes())
        raw[-1] ^= 0xFF
        tampered.write_bytes(bytes(raw))
        try:
            decrypt_file(tampered, tmp / "evil.csv", PASSWORD)
            print("[X] 篡改未被发现（不应发生）")
        except DecryptionError:
            print("[OK] 篡改 1 个字节即被 GCM 校验拒绝，且不产生半截明文")

        # ---- 2. 敏感文件检测 ------------------------------------
        line("2. 敏感文件风险检测")
        scanner = SensitiveScanner(max_file_size=1 << 20, workers=2)
        report = scanner.scan([work], control=ScanControl(), min_risk="low")
        print(f"扫描文件 {report.scanned} 个，风险分布：{report.summary}")
        for risk in report.files:
            labels = "、".join(sorted({f.label for f in risk.findings}))
            print(f"  [{risk.risk.upper():<6}] {risk.score:>3} 分  {Path(risk.path).name}  <- {labels}")
        assert report.summary.get("high", 0) >= 1

        # ---- 3. 审计日志 ----------------------------------------
        line("3. 操作审计日志（逐条加密 + HMAC 哈希链）")
        audit = AuditLog(cfg.audit_dir, PASSWORD, iterations=ITERATIONS)
        audit.record("encrypt", path=src, detail=f"生成 {info['dst']}")
        audit.record("decrypt", path=restored, detail="还原并校验")
        audit.record("scan", detail=f"扫描 {report.scanned} 个文件，高危 {report.summary.get('high', 0)} 个")
        for row in audit.tail(10):
            print(f"  {row['ts']}  {row['event']:<8} {row.get('path') or '-'}")
        check = audit.verify()
        print(f"[OK] 哈希链校验通过：{check['records']} 条，链头 {check['head'][:16]}...")
        print(f"     日志文件：{cfg.audit_dir / 'audit.jsonl'}（打开看只有密文）")

        # ---- 4. 加密备份 / 版本回滚 / 误删恢复 -------------------
        line("4. 安全备份与恢复（增量 + 加密 + 版本）")
        store = BackupStore(
            cfg.backup_dir,
            PASSWORD,
            iterations=ITERATIONS,
            chunk_size=CHUNK_SIZE,
            log_iterations=ITERATIONS,
        )
        v1 = store.backup_file(src)
        src.write_text("姓名,身份证号\n王五,110105199505051234\n", encoding="utf-8")
        v2 = store.backup_file(src)
        again = store.backup_file(src)
        print(f"[OK] 备份 v{v1['version']} -> v{v2['version']}；内容未变时再次备份：{again['skipped']}（增量去重）")
        source_copy = tmp / "rollback.csv"
        store.restore(src, version=1, dest=source_copy)
        print(f"[OK] 回滚到 v1，恢复内容首行：{source_copy.read_text(encoding='utf-8').splitlines()[0]}")
        src.unlink()
        recovered = store.restore(src)
        print(f"[OK] 原文件删除后从备份恢复：{recovered['restored_to']}")
        verify = store.verify(full=True)
        print(f"[OK] 备份完整性校验：索引 {verify['index_records']} 条、对象 {verify['objects_checked']} 个，通过={verify['ok']}")
        print(f"     备份对象是加密容器：{next((cfg.backup_dir / 'objects').rglob('*.fgc'))}")

        line("完成")
        print("所有敏感运算都在本机完成，原始文件没有离开过这台电脑。")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
