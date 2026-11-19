#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""export_credentials.py と m0_m1.sh のテスト。

- 合成 NVS イメージはテスト内で組み立てる (ESP-IDF 不要)。壊れたイメージ・
  キー欠落・空値・旧キー・複数ページ・フルフラッシュ切り出しを検証する。
- ESP-IDF の ``nvs_partition_gen.py`` を使うテストは、IDF の python env が
  見つかったときだけ実行する (無ければ明示的に skip)。

実行:
  python3 -m unittest discover -s tools/muse-gadget/test -v
  (SAS-Watch リポジトリのルートで実行)
"""

from __future__ import annotations

import json
import os
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# tools/muse-gadget を import パスに入れる。
TOOL_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = Path(__file__).resolve().parents[3]
WORKSPACE = REPO_ROOT.parent
sys.path.insert(0, str(TOOL_DIR))

import export_credentials as ec  # noqa: E402

SCRIPT = TOOL_DIR / "export_credentials.py"
RUNBOOK = TOOL_DIR / "m0_m1.sh"

PAGE = 4096
ENTRY = 32
ENTRIES = 126

# テスト用のダミー値 (実在の資格情報ではない)。画面/ログに出ないことの検証に使う。
DUMMY_HOST = "host-EXAMPLE.invalid"
DUMMY_VM = "vm-EXAMPLE-0001"
DUMMY_TOKEN = "DUMMY-TOKEN-0123456789-abcdefghijklmnopqrstuvwxyz"
DUMMY_SSID = "Dummy WiFi Net"
DUMMY_PASS = "Dummy Pass 9876"
SECRETS = (DUMMY_HOST, DUMMY_VM, DUMMY_TOKEN, DUMMY_SSID, DUMMY_PASS)


# ---- 合成 NVS イメージの組み立て (ESP-IDF 不要) ---------------------------


def _entry(ns: int, entry_type: int, span: int, key: str, data8: bytes) -> bytes:
    raw = bytes([ns, entry_type, span, 0xFF]) + b"\x00" * 4
    raw += key.encode("ascii").ljust(16, b"\x00")[:16]
    raw += data8.ljust(8, b"\xff")[:8]
    crc = zlib.crc32(raw[:4] + raw[8:32], 0xFFFFFFFF)
    return raw[:4] + struct.pack("<I", crc) + raw[8:32]


def _string_entries(ns: int, key: str, value: bytes) -> List[bytes]:
    """string 型の先頭エントリ + 続きエントリ (生データ) を作る。"""
    payload = value + b"\x00"
    span = 1 + (len(payload) + ENTRY - 1) // ENTRY
    data8 = struct.pack("<H", len(payload)) + b"\xff\xff" + struct.pack(
        "<I", zlib.crc32(payload, 0xFFFFFFFF)
    )
    entries = [_entry(ns, 0x21, span, key, data8)]
    padded = payload + b"\xff" * ((span - 1) * ENTRY - len(payload))
    for chunk in range(span - 1):
        entries.append(padded[chunk * ENTRY : (chunk + 1) * ENTRY])
    return entries


def _page(entries: List[bytes], page_index: int, status: int) -> bytes:
    header = struct.pack("<II", status, page_index) + b"\xfe" + b"\xff" * 19
    header += struct.pack("<I", zlib.crc32(header[4:28], 0xFFFFFFFF))

    bitmap = bytearray(b"\xff" * 32)  # Empty (0b11)
    for index in range(len(entries)):
        shift = (index % 4) * 2
        bitmap[index // 4] = (bitmap[index // 4] & ~(0b11 << shift)) | (0b10 << shift)

    table = bytearray(b"\xff" * (ENTRIES * ENTRY))
    for index, raw in enumerate(entries):
        assert len(raw) == ENTRY
        table[index * ENTRY : (index + 1) * ENTRY] = raw

    page = header + bytes(bitmap) + bytes(table)
    assert len(page) == PAGE
    return page


def build_nvs(
    namespace: Optional[str] = "muse",
    ns_index: int = 1,
    host: Optional[bytes] = DUMMY_HOST.encode(),
    vm: Optional[bytes] = DUMMY_VM.encode(),
    token: Optional[bytes] = DUMMY_TOKEN.encode(),
    vm_key: str = "vm",
    extra_items: Optional[List[Tuple[str, bytes]]] = None,
    pages: int = 1,
) -> bytes:
    """1 ページ (または複数ページ) の NVS イメージを組み立てる。"""
    entries: List[bytes] = []
    if namespace is not None:
        entries.append(_entry(0, 0x01, 1, namespace, bytes([ns_index]) + b"\xff" * 7))
    if host is not None:
        entries += _string_entries(ns_index, "host", host)
    if vm is not None:
        entries += _string_entries(ns_index, vm_key, vm)
    if token is not None:
        entries += _string_entries(ns_index, "token", token)
    for key, value in extra_items or []:
        entries += _string_entries(ns_index, key, value)

    result = _page(entries, 0, ec.PAGE_STATUS_ACTIVE)
    assert pages >= 1
    # 2 ページ目以降は空ページ (状態は Free)。
    result += b"\xff" * PAGE * (pages - 1)
    return result


def write_temp_image(directory: Path, data: bytes, name: str = "nvs.bin") -> Path:
    path = directory / name
    path.write_bytes(data)
    return path


def run_tool(args: List[str], cwd: Optional[Path] = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        capture_output=True,
        text=True,
        cwd=str(cwd or REPO_ROOT),
    )


def assert_no_secrets(test: unittest.TestCase, text: str, where: str = "出力") -> None:
    for secret in SECRETS:
        test.assertNotIn(secret, text, "%s に資格情報の値が漏れています" % where)


# ---- パーサ / 抽出 (ESP-IDF 不要) -----------------------------------------


class ParserTests(unittest.TestCase):
    def test_parses_strings_and_namespace(self):
        parsed = ec.parse_nvs(build_nvs())
        self.assertIn("muse", parsed.namespaces)
        ns = parsed.namespaces["muse"]
        self.assertEqual(ns["host"].kind, "string")
        self.assertEqual(ns["host"].data, DUMMY_HOST.encode())

    def test_extract_and_lengths(self):
        parsed = ec.parse_nvs(build_nvs())
        creds = ec.extract_credentials(parsed)
        self.assertEqual(creds.host, DUMMY_HOST)
        self.assertEqual(creds.vm, DUMMY_VM)
        self.assertEqual(creds.token, DUMMY_TOKEN)
        self.assertFalse(creds.vm_legacy)

    def test_legacy_vm_id_is_accepted(self):
        image = build_nvs(vm=DUMMY_VM.encode(), vm_key="vm_id")
        statuses = ec.inspect_credentials(ec.parse_nvs(image))
        self.assertTrue(statuses["vm"].legacy)
        creds = ec.extract_credentials(ec.parse_nvs(image))
        self.assertEqual(creds.vm, DUMMY_VM)
        self.assertTrue(creds.vm_legacy)

    def test_missing_namespace(self):
        image = build_nvs(namespace="other")
        statuses = ec.inspect_credentials(ec.parse_nvs(image))
        self.assertFalse(any(status.ok for status in statuses.values()))
        with self.assertRaises(ec.CredentialsError) as ctx:
            ec.extract_credentials(ec.parse_nvs(image))
        self.assertIn("muse", str(ctx.exception))

    def test_missing_token(self):
        statuses = ec.inspect_credentials(ec.parse_nvs(build_nvs(token=None)))
        self.assertFalse(statuses["token"].ok)
        self.assertTrue(statuses["host"].ok)
        with self.assertRaises(ec.CredentialsError):
            ec.extract_credentials(ec.parse_nvs(build_nvs(token=None)))

    def test_empty_token(self):
        image = build_nvs(token=b"")
        statuses = ec.inspect_credentials(ec.parse_nvs(image))
        self.assertFalse(statuses["token"].ok)
        self.assertIn("空", statuses["token"].error)
        with self.assertRaises(ec.CredentialsError):
            ec.extract_credentials(ec.parse_nvs(image))

    def test_space_in_token_is_rejected(self):
        image = build_nvs(token=b"bad token")
        statuses = ec.inspect_credentials(ec.parse_nvs(image))
        self.assertFalse(statuses["token"].ok)
        self.assertIn("空白", statuses["token"].error)

    def test_too_long_token_is_rejected(self):
        image = build_nvs(token=b"T" * (ec.TOKEN_MAX + 1))
        statuses = ec.inspect_credentials(ec.parse_nvs(image))
        self.assertFalse(statuses["token"].ok)
        self.assertIn("長すぎ", statuses["token"].error)

    def test_max_length_token_is_accepted(self):
        token = b"T" * ec.TOKEN_MAX
        creds = ec.extract_credentials(ec.parse_nvs(build_nvs(token=token)))
        self.assertEqual(len(creds.token), ec.TOKEN_MAX)

    def test_latest_page_wins(self):
        first = build_nvs(host=b"old.example.invalid")
        second = _page(_string_entries(1, "host", b"new.example.invalid"), 1, ec.PAGE_STATUS_ACTIVE)
        # 1 ページ目の状態を Full にして、2 ページ目に新しい値を置く。
        first_page = bytearray(first[:PAGE])
        first_page[0:4] = struct.pack("<I", ec.PAGE_STATUS_FULL)
        first_page[28:32] = struct.pack(
            "<I", zlib.crc32(bytes(first_page[4:28]), 0xFFFFFFFF)
        )
        image = bytes(first_page) + second
        creds = ec.extract_credentials(ec.parse_nvs(image))
        self.assertEqual(creds.host, "new.example.invalid")

    def test_entry_crc_corruption_is_detected(self):
        image = bytearray(build_nvs())
        # namespace エントリの key 領域を壊す (entry CRC 不一致)。
        image[64 + 8] ^= 0xFF
        with self.assertRaises(ec.NVSCorruptError) as ctx:
            ec.parse_nvs(bytes(image))
        self.assertIn("CRC", str(ctx.exception))

    def test_header_crc_corruption_is_detected(self):
        image = bytearray(build_nvs())
        image[16] ^= 0xFF  # ページヘッダのパディング
        with self.assertRaises(ec.NVSCorruptError) as ctx:
            ec.parse_nvs(bytes(image))
        self.assertIn("ヘッダ", str(ctx.exception))

    def test_string_payload_corruption_is_detected(self):
        image = bytearray(build_nvs())
        # 最初の string の続きエントリ (host の先頭データ) を壊す。
        # レイアウト: header 32 + bitmap 32 + namespace entry + host entry + child
        child_offset = 64 + 2 * ENTRY
        image[child_offset] ^= 0xFF
        with self.assertRaises(ec.NVSCorruptError) as ctx:
            ec.parse_nvs(bytes(image))
        self.assertIn("CRC", str(ctx.exception))

    def test_non_page_aligned_image_is_rejected(self):
        with self.assertRaises(ec.NVSCorruptError) as ctx:
            ec.parse_nvs(b"\x00" * 1234)
        self.assertIn("倍数", str(ctx.exception))

    def test_empty_image_is_rejected(self):
        with self.assertRaises(ec.NVSCorruptError):
            ec.parse_nvs(b"")

    def test_all_ff_image_is_rejected(self):
        with self.assertRaises(ec.NVSCorruptError):
            ec.parse_nvs(b"\xff" * PAGE)


class SliceTests(unittest.TestCase):
    def test_slice_from_flash(self):
        nvs = build_nvs()
        flash = bytearray(b"\xff" * 0x6000)
        offset = 0x1000
        flash[offset : offset + len(nvs)] = nvs
        sliced = ec.slice_nvs_from_flash(bytes(flash), offset, len(nvs))
        self.assertEqual(sliced, nvs)

    def test_slice_out_of_range(self):
        with self.assertRaises(ec.NVSError):
            ec.slice_nvs_from_flash(b"\x00" * 0x1000, 0x800, 0x1000)

    def test_slice_non_aligned_size(self):
        with self.assertRaises(ec.NVSError):
            ec.slice_nvs_from_flash(b"\x00" * 0x1000, 0, 0x1001)

    def test_nvs_file_passed_as_flash_has_hint(self):
        nvs = build_nvs()
        with self.assertRaises(ec.NVSError) as ctx:
            ec.slice_nvs_from_flash(nvs, 0x11000, 0xC000)
        self.assertIn("--nvs", str(ctx.exception))


# ---- CLI (サブプロセス) ---------------------------------------------------


class CliTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.nvs_path = write_temp_image(self.tmp, build_nvs())

    def tearDown(self):
        self._tmp.cleanup()

    def test_help(self):
        result = run_tool(["--help"])
        self.assertEqual(result.returncode, 0)
        self.assertIn("--nvs", result.stdout)
        self.assertIn("--out", result.stdout)

    def test_check_ok_and_no_values(self):
        result = run_tool(["--nvs", str(self.nvs_path), "--check"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('namespace "muse": あり', result.stdout)
        self.assertIn("OK (3/3)", result.stdout)
        self.assertIn("host", result.stdout)
        self.assertIn("token", result.stdout)
        assert_no_secrets(self, result.stdout)
        assert_no_secrets(self, result.stderr)

    def test_check_ng_exit_code_and_no_values(self):
        path = write_temp_image(self.tmp, build_nvs(token=None), "missing.bin")
        result = run_tool(["--nvs", str(path), "--check"])
        self.assertEqual(result.returncode, 1)
        self.assertIn("NG (2/3)", result.stdout)
        assert_no_secrets(self, result.stdout)
        assert_no_secrets(self, result.stderr)

    def test_export_json_and_shell_files(self):
        out = self.tmp / "creds.json"
        shell = self.tmp / "muse-set.txt"
        result = run_tool(
            ["--nvs", str(self.nvs_path), "--out", str(out), "--shell", str(shell)]
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        assert_no_secrets(self, result.stdout)
        assert_no_secrets(self, result.stderr)

        for path in (out, shell):
            self.assertTrue(path.exists())
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600, str(path))

        payload = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(payload["host"], DUMMY_HOST)
        self.assertEqual(payload["vm"], DUMMY_VM)
        self.assertEqual(payload["token"], DUMMY_TOKEN)
        self.assertEqual(payload["lengths"]["token"], len(DUMMY_TOKEN))

        lines = shell.read_text(encoding="utf-8").splitlines()
        self.assertEqual(
            lines,
            [
                "muse set host %s" % DUMMY_HOST,
                "muse set vm %s" % DUMMY_VM,
                "muse set token %s" % DUMMY_TOKEN,
                "muse status",
            ],
        )

    def test_export_refuses_overwrite_without_force(self):
        out = self.tmp / "creds.json"
        first = run_tool(["--nvs", str(self.nvs_path), "--out", str(out)])
        self.assertEqual(first.returncode, 0, first.stderr)
        second = run_tool(["--nvs", str(self.nvs_path), "--out", str(out)])
        self.assertEqual(second.returncode, 1)
        self.assertIn("--force", second.stderr)
        forced = run_tool(["--nvs", str(self.nvs_path), "--out", str(out), "--force"])
        self.assertEqual(forced.returncode, 0, forced.stderr)

    def test_export_fails_without_token_and_prints_no_values(self):
        path = write_temp_image(self.tmp, build_nvs(token=None), "missing.bin")
        out = self.tmp / "creds.json"
        result = run_tool(["--nvs", str(path), "--out", str(out)])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(out.exists())
        self.assertIn("資格情報", result.stderr)
        assert_no_secrets(self, result.stdout)
        assert_no_secrets(self, result.stderr)

    def test_corrupt_image_error_and_no_values(self):
        image = bytearray(build_nvs())
        image[64 + 8] ^= 0xFF
        path = write_temp_image(self.tmp, bytes(image), "corrupt.bin")
        result = run_tool(["--nvs", str(path), "--check"])
        self.assertEqual(result.returncode, 1)
        self.assertIn("エラー", result.stderr)
        assert_no_secrets(self, result.stdout)
        assert_no_secrets(self, result.stderr)

    def test_flash_input_slicing(self):
        nvs = build_nvs()
        offset = 0x2000
        flash = bytearray(b"\xff" * 0x8000)
        flash[offset : offset + len(nvs)] = nvs
        path = write_temp_image(self.tmp, bytes(flash), "flash.bin")
        result = run_tool(
            [
                "--flash",
                str(path),
                "--nvs-offset",
                hex(offset),
                "--nvs-size",
                hex(len(nvs)),
                "--check",
            ]
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("OK (3/3)", result.stdout)

    def test_flash_input_with_default_offset_in_32mb_file(self):
        nvs = build_nvs()
        flash = bytearray(b"\xff" * 0x2000000)
        flash[ec.DEFAULT_NVS_OFFSET : ec.DEFAULT_NVS_OFFSET + len(nvs)] = nvs
        path = write_temp_image(self.tmp, bytes(flash), "flash-32m.bin")
        result = run_tool(["--flash", str(path), "--check"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("49152 バイト", result.stdout)

    def test_missing_input_file(self):
        result = run_tool(["--nvs", str(self.tmp / "nope.bin"), "--check"])
        self.assertEqual(result.returncode, 1)
        self.assertIn("読めません", result.stderr)

    def test_nvs_option_is_required(self):
        result = run_tool(["--check"])
        self.assertEqual(result.returncode, 2)

    def test_quiet_default_is_check(self):
        result = run_tool(["--nvs", str(self.nvs_path)])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("結果: OK", result.stdout)
        assert_no_secrets(self, result.stdout)


# ---- ランブック (m0_m1.sh) ------------------------------------------------


class RunbookTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.nvs_path = write_temp_image(self.tmp, build_nvs())

    def tearDown(self):
        self._tmp.cleanup()

    def run_runbook(self, args: List[str]) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["bash", str(RUNBOOK), *args],
            capture_output=True,
            text=True,
            cwd=str(REPO_ROOT),
        )

    def test_help(self):
        result = self.run_runbook(["--help"])
        self.assertEqual(result.returncode, 0)
        self.assertIn("--dump-nvs", result.stdout)
        self.assertIn("dry-run", result.stdout)

    def test_default_is_dry_run_checklist(self):
        result = self.run_runbook([])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("dry-run", result.stdout)
        self.assertIn("M0 → M1 チェックリスト", result.stdout)

    def test_unknown_option(self):
        result = self.run_runbook(["--bogus"])
        self.assertEqual(result.returncode, 2)

    def test_check_action_with_fixture(self):
        result = self.run_runbook(["--check", "--from", str(self.nvs_path)])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("結果: OK (3/3)", result.stdout)
        assert_no_secrets(self, result.stdout)
        assert_no_secrets(self, result.stderr)

    def test_dump_nvs_dry_run(self):
        result = self.run_runbook(["--dump-nvs"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("read-flash", result.stdout)
        self.assertIn("0x11000", result.stdout)
        self.assertIn("0xC000", result.stdout)

    def test_export_dry_run_does_not_write_credentials(self):
        result = self.run_runbook(["--export", "--from", str(self.nvs_path)])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("[dry-run]", result.stdout)
        self.assertFalse((TOOL_DIR / "out" / "credentials").exists())
        assert_no_secrets(self, result.stdout)
        assert_no_secrets(self, result.stderr)


# ---- ESP-IDF の nvs_partition_gen.py を使うテスト (無ければ skip) ----------


def _find_idf_generator() -> Optional[Path]:
    candidates: List[Path] = []
    env_idf = os.environ.get("IDF_PATH")
    if env_idf:
        candidates.append(Path(env_idf))
    candidates += [
        WORKSPACE / ".toolchain" / "esp-idf",
        WORKSPACE / ".toolchain" / "esp-idf-v6.0.3",
        WORKSPACE / "muse-gadget-test" / "esp-idf-v6.0.3",
        Path.home() / "esp" / "esp-idf",
    ]
    for base in candidates:
        generator = base / "components" / "nvs_flash" / "nvs_partition_generator" / "nvs_partition_gen.py"
        if generator.is_file():
            return generator
    return None


def _find_idf_python() -> Optional[Path]:
    candidates: List[Path] = []
    env_python = os.environ.get("IDF_PYTHON")
    if env_python:
        candidates.append(Path(env_python))
    for pattern in (
        ".toolchain/espressif*/python_env/*/bin/python",
        "muse-gadget-test/espressif*/python_env/*/bin/python",
    ):
        candidates += sorted(WORKSPACE.glob(pattern))
    candidates += sorted(Path.home().glob(".espressif/python_env/*/bin/python"))
    candidates.append(Path(sys.executable))
    for candidate in candidates:
        try:
            result = subprocess.run(
                [str(candidate), "-c", "import esp_idf_nvs_partition_gen"],
                capture_output=True,
                timeout=20,
            )
        except (OSError, subprocess.SubprocessError):
            continue
        if result.returncode == 0:
            return candidate
    return None


class GeneratedNVSTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.generator = _find_idf_generator()
        cls.python = _find_idf_python()
        if cls.generator is None or cls.python is None:
            raise unittest.SkipTest(
                "ESP-IDF の nvs_partition_gen.py / python env が見つからないため skip "
                "(IDF_PATH か .toolchain/esp-idf を用意する)"
            )

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def generate(self, csv_text: str, name: str = "nvs.bin") -> Path:
        csv_path = self.tmp / "muse.csv"
        csv_path.write_text(csv_text, encoding="utf-8")
        out = self.tmp / name
        result = subprocess.run(
            [str(self.python), str(self.generator), "generate", str(csv_path), str(out), "0xC000"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, "nvs_partition_gen failed: %s" % result.stderr)
        self.assertTrue(out.exists())
        return out

    def csv(self, token: Optional[str] = DUMMY_TOKEN) -> str:
        lines = ["key,type,encoding,value", "muse,namespace,,"]
        lines.append("host,data,string,%s" % DUMMY_HOST)
        lines.append("vm,data,string,%s" % DUMMY_VM)
        if token is not None:
            lines.append("token,data,string,%s" % token)
        return "\n".join(lines) + "\n"

    def test_generated_check_ok_and_no_values(self):
        nvs = self.generate(self.csv())
        result = run_tool(["--nvs", str(nvs), "--check"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('namespace "muse": あり', result.stdout)
        self.assertIn("OK (3/3)", result.stdout)
        self.assertIn("token: あり (%d バイト)" % len(DUMMY_TOKEN), result.stdout)
        assert_no_secrets(self, result.stdout)
        assert_no_secrets(self, result.stderr)

    def test_generated_export_matches_inputs(self):
        nvs = self.generate(self.csv())
        out = self.tmp / "creds.json"
        shell = self.tmp / "muse-set.txt"
        result = run_tool(["--nvs", str(nvs), "--out", str(out), "--shell", str(shell)])
        self.assertEqual(result.returncode, 0, result.stderr)
        assert_no_secrets(self, result.stdout)
        payload = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(payload["host"], DUMMY_HOST)
        self.assertEqual(payload["vm"], DUMMY_VM)
        self.assertEqual(payload["token"], DUMMY_TOKEN)
        self.assertEqual(stat.S_IMODE(out.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(shell.stat().st_mode), 0o600)

    def test_generated_max_length_token(self):
        token = "T" * ec.TOKEN_MAX
        nvs = self.generate(self.csv(token=token))
        result = run_tool(["--nvs", str(nvs), "--check"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("token: あり (1023 バイト)", result.stdout)

    def test_generated_missing_token(self):
        nvs = self.generate(self.csv(token=None))
        result = run_tool(["--nvs", str(nvs), "--check"])
        self.assertEqual(result.returncode, 1)
        self.assertIn("NG (2/3)", result.stdout)
        assert_no_secrets(self, result.stdout)

    def test_generated_other_namespace(self):
        csv_text = "\n".join(
            [
                "key,type,encoding,value",
                "other,namespace,,",
                "host,data,string,%s" % DUMMY_HOST,
                "vm,data,string,%s" % DUMMY_VM,
                "token,data,string,%s" % DUMMY_TOKEN,
            ]
        ) + "\n"
        nvs = self.generate(csv_text)
        result = run_tool(["--nvs", str(nvs), "--check"])
        self.assertEqual(result.returncode, 1)
        self.assertIn('namespace "muse": なし', result.stdout)

    def test_generated_image_in_flash_layout(self):
        nvs_path = self.generate(self.csv())
        nvs = nvs_path.read_bytes()
        flash = bytearray(b"\xff" * (ec.DEFAULT_NVS_OFFSET + len(nvs)))
        flash[ec.DEFAULT_NVS_OFFSET :] = nvs
        flash_path = self.tmp / "full.bin"
        flash_path.write_bytes(bytes(flash))
        result = run_tool(["--flash", str(flash_path), "--check"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("OK (3/3)", result.stdout)

    def test_generated_corrupt_payload(self):
        nvs = bytearray(self.generate(self.csv()).read_bytes())
        # 文字列の続きエントリ (namespace の次のエントリの直後) を 1 バイト壊す。
        offset = 64 + 2 * ENTRY  # header + bitmap + namespace entry -> first child
        nvs[offset] ^= 0xFF
        path = self.tmp / "corrupt.bin"
        path.write_bytes(bytes(nvs))
        result = run_tool(["--nvs", str(path), "--check"])
        self.assertEqual(result.returncode, 1)
        self.assertIn("CRC", result.stderr)
        assert_no_secrets(self, result.stdout)
        assert_no_secrets(self, result.stderr)

    def test_generated_truncated_image(self):
        nvs = self.generate(self.csv()).read_bytes()[:-1]
        path = self.tmp / "truncated.bin"
        path.write_bytes(nvs)
        result = run_tool(["--nvs", str(path), "--check"])
        self.assertEqual(result.returncode, 1)
        self.assertIn("倍数", result.stderr)


# ---- シリアル投入スクリプト (m0_m1.sh 埋め込み) の擬似シリアルテスト ----------


def _extract_embedded_feeder() -> str:
    """m0_m1.sh の heredoc から、シリアル投入用 Python を取り出す。"""
    text = RUNBOOK.read_text(encoding="utf-8")
    marker = "<<'PY'\n"
    start = text.index(marker) + len(marker)
    end = text.index("\nPY\n", start)
    return text[start:end]


FAKE_SERIAL = r'''import json
import os


class SerialException(Exception):
    pass


class Serial:
    """m0_m1.sh --feed-serial の相手をする擬似ファームウェア。

    'M' で行モードに入り、改行で 1 行を受理してログを返す。
    FAKE_SERIAL_MODE=no_line_ack では受理ログを返さない (安全に中止するか)。
    """

    def __init__(self, port=None, baudrate=115200, timeout=0.2, write_timeout=2,
                 dsrdtr=False, rtscts=False):
        self.read_buf = b""
        self.mode = False
        self.current = b""
        self.written = []
        self.mode_setting = os.environ.get("FAKE_SERIAL_MODE", "ok")
        self.token_len = int(os.environ.get("FAKE_SERIAL_TOKEN_LEN", "0"))
        self.log_path = os.environ.get("FAKE_SERIAL_LOG")
        self.dtr = False
        self.rts = False

    def reset_input_buffer(self):
        self.read_buf = b""

    def read(self, size):
        data, self.read_buf = self.read_buf[:size], self.read_buf[size:]
        return data

    def flush(self):
        pass

    def write(self, data):
        for byte in data:
            if self.mode:
                if byte in (0x0A, 0x0D):
                    line = self.current
                    self.current = b""
                    self.mode = False
                    self.written.append(line.decode("utf-8", "replace"))
                    if self.mode_setting != "no_line_ack":
                        self.read_buf += b"serial: muse line (%d bytes)\n" % len(line)
                        if line == b"muse status":
                            self.read_buf += (
                                b'{"configured":true,"ready":false,"wifi":false,"token_len":%d}\n'
                                % self.token_len
                            )
                else:
                    self.current += bytes([byte])
            elif byte == ord("M"):
                self.mode = True
                self.current = b""
                self.read_buf += b"serial: muse line mode ('M'): type \"muse ...\" then Enter\n"
        return len(data)

    def close(self):
        if self.log_path:
            with open(self.log_path, "w", encoding="utf-8") as handle:
                json.dump(self.written, handle)
'''


class SerialFeedTests(unittest.TestCase):
    """シリアル投入スクリプトを擬似ファームウェアで検証する (実機不要)。"""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.feeder = self.tmp / "feeder.py"
        self.feeder.write_text(_extract_embedded_feeder(), encoding="utf-8")
        (self.tmp / "serial.py").write_text(FAKE_SERIAL, encoding="utf-8")

        self.shell = self.tmp / "muse-set.txt"
        self.shell.write_text(
            "\n".join(
                [
                    "muse set host %s" % DUMMY_HOST,
                    "muse set vm %s" % DUMMY_VM,
                    "muse set token %s" % DUMMY_TOKEN,
                    "muse status",
                ]
            )
            + "\n",
            encoding="utf-8",
        )
        self.creds = self.tmp / "creds.json"
        self.creds.write_text(
            json.dumps({"host": DUMMY_HOST, "vm": DUMMY_VM, "token": DUMMY_TOKEN}),
            encoding="utf-8",
        )
        self.log = self.tmp / "written.json"

    def tearDown(self):
        self._tmp.cleanup()

    def run_feeder(self, mode="ok", token_len=None):
        env = dict(os.environ)
        env["PYTHONPATH"] = str(self.tmp)
        env["FAKE_SERIAL_MODE"] = mode
        env["FAKE_SERIAL_LOG"] = str(self.log)
        env["FAKE_SERIAL_TOKEN_LEN"] = str(len(DUMMY_TOKEN) if token_len is None else token_len)
        return subprocess.run(
            [sys.executable, str(self.feeder), "FAKE", "115200", str(self.shell), str(self.creds)],
            capture_output=True,
            text=True,
            env=env,
            timeout=60,
        )

    def test_embedded_script_compiles(self):
        compile(_extract_embedded_feeder(), "feeder.py", "exec")

    def test_success_sends_one_m_per_line(self):
        result = self.run_feeder()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("token_len=%d" % len(DUMMY_TOKEN), result.stdout)
        self.assertIn("投入に成功", result.stdout)
        assert_no_secrets(self, result.stdout)
        assert_no_secrets(self, result.stderr)
        written = json.loads(self.log.read_text(encoding="utf-8"))
        self.assertEqual(
            written,
            [
                "0",  # 安全確認用の副作用なし行
                "muse set host %s" % DUMMY_HOST,
                "muse set vm %s" % DUMMY_VM,
                "muse set token %s" % DUMMY_TOKEN,
                "muse status",
            ],
        )

    def test_aborts_before_secrets_when_line_mode_does_not_answer(self):
        result = self.run_feeder(mode="no_line_ack")
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn("資格情報は送信していません", result.stderr)
        assert_no_secrets(self, result.stdout)
        assert_no_secrets(self, result.stderr)
        written = json.loads(self.log.read_text(encoding="utf-8"))
        self.assertEqual(written, ["0"])
        for line in written:
            self.assertNotIn("muse set", line)

    def test_token_len_mismatch_is_detected(self):
        result = self.run_feeder(token_len=len(DUMMY_TOKEN) - 1)
        self.assertEqual(result.returncode, 6, result.stderr)
        self.assertIn("一致しません", result.stderr)
        assert_no_secrets(self, result.stdout)
        assert_no_secrets(self, result.stderr)


class HomehubAndWifiTests(unittest.TestCase):
    """公式 Muse ファームの namespace ``homehub`` と ``--include-wifi`` の検証。"""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def build_homehub(
        self,
        *,
        access_token: Optional[bytes] = DUMMY_TOKEN.encode(),
        auth_token: Optional[bytes] = None,
        noise_host: Optional[bytes] = DUMMY_HOST.encode(),
        vm_url: Optional[bytes] = None,
        ssid: Optional[bytes] = DUMMY_SSID.encode(),
        password: Optional[bytes] = DUMMY_PASS.encode(),
        also_muse_non_token: bool = True,
    ) -> Path:
        entries: List[bytes] = []
        entries.append(_entry(0, 0x01, 1, "homehub", bytes([1]) + b"\xff" * 7))
        if access_token is not None:
            entries += _string_entries(1, "access_token", access_token)
        if auth_token is not None:
            entries += _string_entries(1, "auth_token", auth_token)
        if noise_host is not None:
            entries += _string_entries(1, "noise_host", noise_host)
        if vm_url is not None:
            entries += _string_entries(1, "vm_url", vm_url)
        if ssid is not None:
            entries += _string_entries(1, "ssid", ssid)
        if password is not None:
            entries += _string_entries(1, "password", password)
        if also_muse_non_token:
            entries.append(_entry(0, 0x01, 1, "muse", bytes([2]) + b"\xff" * 7))
            entries.append(_entry(2, 0x01, 1, "volume", bytes([80]) + b"\xff" * 7))
        return write_temp_image(self.tmp, _page(entries, 0, ec.PAGE_STATUS_ACTIVE), "homehub.bin")

    def test_homehub_defaults_and_include_wifi_export(self):
        path = self.build_homehub(noise_host=None, vm_url=None)
        out = self.tmp / "creds.json"
        shell = self.tmp / "muse-set.txt"
        result = run_tool(
            ["--nvs", str(path), "--include-wifi", "--check", "--out", str(out), "--shell", str(shell)]
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('namespace "homehub": あり', result.stdout)
        self.assertIn("結果: OK (5/5)", result.stdout)
        assert_no_secrets(self, result.stdout)
        assert_no_secrets(self, result.stderr)

        payload = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(payload["host"], ec.DEFAULT_HOST)
        self.assertEqual(payload["vm"], "")
        self.assertEqual(payload["token"], DUMMY_TOKEN)
        self.assertEqual(payload["source_namespace"], "homehub")
        self.assertEqual(payload["wifi_ssid"], DUMMY_SSID)
        self.assertEqual(payload["wifi_pass"], DUMMY_PASS)
        self.assertEqual(payload["lengths"]["wifi_ssid"], len(DUMMY_SSID))
        self.assertEqual(payload["lengths"]["wifi_pass"], len(DUMMY_PASS))

        lines = shell.read_text(encoding="utf-8").splitlines()
        self.assertEqual(
            lines,
            [
                "muse set ssid %s" % DUMMY_SSID,
                "muse set pass %s" % DUMMY_PASS,
                "muse set host %s" % ec.DEFAULT_HOST,
                "muse set vm",
                "muse set token %s" % DUMMY_TOKEN,
                "muse status",
            ],
        )

    def test_homehub_vm_url_derivation_and_auth_token_fallback(self):
        path = self.build_homehub(
            access_token=None,
            auth_token=DUMMY_TOKEN.encode(),
            vm_url=("wss://%s.metaaivm.com/v1/noise" % DUMMY_VM).encode(),
        )
        parsed = ec.parse_nvs(path.read_bytes())
        creds = ec.extract_credentials(parsed)
        self.assertEqual(creds.host, DUMMY_HOST)
        self.assertEqual(creds.vm, DUMMY_VM)
        self.assertEqual(creds.token, DUMMY_TOKEN)
        self.assertEqual(creds.source_namespace, "homehub")

    def test_homehub_open_wifi_without_password(self):
        path = self.build_homehub(password=None)
        shell = self.tmp / "open-wifi.txt"
        result = run_tool(["--nvs", str(path), "--include-wifi", "--shell", str(shell)])
        self.assertEqual(result.returncode, 0, result.stderr)
        assert_no_secrets(self, result.stdout)
        lines = shell.read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[0], "muse set ssid %s" % DUMMY_SSID)
        self.assertEqual(lines[1], "muse set pass")

    def test_include_wifi_missing_ssid_fails_without_secret_leak(self):
        path = self.build_homehub(ssid=None)
        result = run_tool(["--nvs", str(path), "--include-wifi", "--check"])
        self.assertEqual(result.returncode, 1)
        self.assertIn("NG (4/5)", result.stdout)
        assert_no_secrets(self, result.stdout)
        assert_no_secrets(self, result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
