"""敏感文件检测测试：规则命中、误报控制、Office 文本抽取、暂停/停止/分级。"""

from __future__ import annotations

import io
import tempfile
import threading
import time
import unittest
import zipfile
from pathlib import Path

from helpers import make_bank_card, make_id_card

from file_guard.scan import (
    ScanControl,
    SensitiveScanner,
    is_valid_id_card,
    is_valid_luhn,
    mask_value,
    scan_paths,
)


class ValidatorTest(unittest.TestCase):
    def test_id_card_checksum(self) -> None:
        valid = make_id_card()
        self.assertTrue(is_valid_id_card(valid))
        self.assertFalse(is_valid_id_card(valid[:-1] + ("0" if valid[-1] != "0" else "1")))
        self.assertFalse(is_valid_id_card("1234"))

    def test_luhn(self) -> None:
        card = make_bank_card()
        self.assertTrue(is_valid_luhn(card))
        self.assertFalse(is_valid_luhn(card[:-1] + ("9" if card[-1] != "9" else "8")))

    def test_mask(self) -> None:
        self.assertNotIn("123456", mask_value("110105199001011234"))
        self.assertTrue(mask_value("110105199001011234").startswith("110"))
        self.assertEqual(mask_value("abc"), "***")


class ScanRulesTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _write(self, name: str, content: str) -> Path:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def test_high_risk_content(self) -> None:
        card = make_bank_card()
        self._write("customer.csv", f"姓名,身份证号,手机号,银行卡\n张三,{make_id_card()},13812345678,{card}\n")
        report = scan_paths([self.root])
        self.assertEqual(report.summary["high"], 1)
        risk = report.files[0]
        self.assertEqual(risk.risk, "high")
        kinds = {f.kind for f in risk.findings}
        self.assertTrue({"id_card", "phone", "bank_card"} <= kinds, kinds)
        # 打码后不应出现完整身份证号
        payload = report.to_dict()
        self.assertNotIn(make_id_card(), str(payload))

    def test_contract_and_classified(self) -> None:
        self._write("memo.txt", "甲方：某某科技有限公司\n乙方：另一家公司\n本保密协议自签署之日起生效。\n机密")
        risk = SensitiveScanner().scan_file(self.root / "memo.txt")
        self.assertEqual(risk.risk, "medium")  # 机密=medium，合同=low
        kinds = {f.kind for f in risk.findings}
        self.assertIn("classified", kinds)
        self.assertIn("contract", kinds)

    def test_credentials(self) -> None:
        self._write("config.ini", "password = SuperSecret123\napi_key: sk-abcdef\n")
        risk = SensitiveScanner().scan_file(self.root / "config.ini")
        self.assertEqual(risk.risk, "high")
        self.assertIn("credentials", {f.kind for f in risk.findings})

    def test_no_false_positive(self) -> None:
        # 15 位旧版身份证号（不是 18 位）、普通会议文本，都不应命中
        self._write("clean.txt", "这是一份普通的会议纪要，没有敏感信息。\n旧编号：110105900101123")
        report = scan_paths([self.root])
        self.assertEqual(report.summary["high"], 0)
        self.assertEqual(report.summary["none"], 1)

    def test_filename_rule_for_binary_format(self) -> None:
        (self.root / "2026年劳动合同.pdf").write_bytes(b"%PDF-1.4 binary")
        report = scan_paths([self.root])
        self.assertEqual(report.files[0].risk, "medium")
        self.assertEqual(report.files[0].findings[0].kind, "contract")

    def test_binary_files_skipped_without_error(self) -> None:
        (self.root / "blob.bin").write_bytes(b"\x00\x01" + b"13812345678" + b"\x00")
        report = scan_paths([self.root])
        self.assertEqual(report.summary["findings"], 0)

    def test_office_docx_text_extracted(self) -> None:
        docx = self.root / "resume.docx"
        xml = (
            '<?xml version="1.0" encoding="UTF-8"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
            f"<w:body><w:p><w:t>姓名：张三 身份证 {make_id_card()} 手机 13900001111</w:t></w:p></w:body></w:document>"
        )
        with zipfile.ZipFile(docx, "w") as zf:
            zf.writestr("word/document.xml", xml)
        risk = SensitiveScanner().scan_file(docx)
        self.assertEqual(risk.risk, "high")
        self.assertIn("id_card", {f.kind for f in risk.findings})

    def test_min_risk_filter(self) -> None:
        self._write("low.txt", "甲方：A公司\n乙方：B公司")
        self._write("high.txt", f"身份证 {make_id_card()}")
        report = scan_paths([self.root], min_risk="high")
        self.assertEqual([Path(r.path).name for r in report.files], ["high.txt"])

    def test_oversized_file_skipped_but_named(self) -> None:
        path = self.root / "身份证扫描件.txt"
        path.write_text("x" * 5000, encoding="utf-8")
        risk = SensitiveScanner(max_file_size=1024).scan_file(path)
        self.assertIn("超过", risk.note)
        self.assertTrue(any("身份证" in f.label for f in risk.findings))

    def test_skip_dir_names(self) -> None:
        self._write("node_modules/pkg/index.js", f"身份证 {make_id_card()}")
        self._write("src/app.js", "console.log(1)")
        report = scan_paths([self.root])
        self.assertEqual(report.summary["high"], 0)  # node_modules 被跳过
        self.assertEqual(report.scanned, 1)

    def test_progress_callback(self) -> None:
        for i in range(3):
            self._write(f"f{i}.txt", "hello")
        seen: list[tuple[int, int]] = []
        scan_paths([self.root], progress=lambda done, total, cur: seen.append((done, total)))
        self.assertEqual(len(seen), 3)
        self.assertEqual(seen[-1], (3, 3))


class ScanControlTest(unittest.TestCase):
    def test_stop_before_scan(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "a.txt").write_text("hello", encoding="utf-8")
            control = ScanControl()
            control.stop()
            report = scan_paths([tmp], control=control)
            self.assertTrue(report.cancelled)
            self.assertEqual(report.scanned, 0)

    def test_checkpoint_pause_and_resume(self) -> None:
        control = ScanControl()
        control.pause()
        finished = threading.Event()

        def worker() -> None:
            control.checkpoint()
            finished.set()

        thread = threading.Thread(target=worker)
        thread.start()
        time.sleep(0.2)
        self.assertFalse(finished.is_set())  # 暂停中
        control.resume()
        self.assertTrue(finished.wait(2))
        thread.join()

    def test_checkpoint_stop_raises(self) -> None:
        control = ScanControl()
        control.stop()
        with self.assertRaises(Exception):
            control.checkpoint()


class ScanReportTest(unittest.TestCase):
    def test_report_json_serializable(self) -> None:
        import json

        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "a.txt").write_text(f"身份证 {make_id_card()}", encoding="utf-8")
            report = scan_paths([tmp])
            data = json.loads(json.dumps(report.to_dict(), ensure_ascii=False))
            self.assertEqual(data["summary"]["high"], 1)
            self.assertEqual(data["files"][0]["risk_label"], "高")


if __name__ == "__main__":
    unittest.main(verbosity=2)
