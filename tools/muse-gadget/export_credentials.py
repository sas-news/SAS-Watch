#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""公式 Muse Gadget ファームの NVS から Muse の資格情報を取り出す。

M0 (公式ファーム + Muse アプリでペアリング) の後、公式ファームは NVS
(namespace ``muse``) に ``host`` / ``vm`` / ``token`` を保存している。
このスクリプトはその NVS を読み、値を **画面に出さずに** SAS-Watch へ投入する
ためのファイル (JSON / シリアルコマンド列) を作る。

NVS の読み出しは ESP-IDF の ``nvs_tool.py`` に依存せず、標準ライブラリだけで
最小パースする (ESP-IDF の ``nvs_flash/nvs_partition_tool/nvs_parser.py`` の
形式を参考)。ESP-IDF の入っていない PC でも動く。

使い方 (詳細は ``--help``)::

    # NVS パーティション単体 (48KB) を調べる。値は表示しない。
    python3 tools/muse-gadget/export_credentials.py --nvs muse-nvs.bin --check

    # 32MB のフルフラッシュから NVS 部分 (既定 0x11000 / 0xC000) を切り出して読む
    python3 tools/muse-gadget/export_credentials.py --flash backup.bin --check

    # 値を JSON とシリアル投入用コマンド列に書き出す (0600)
    python3 tools/muse-gadget/export_credentials.py --nvs muse-nvs.bin \
        --out out/credentials/muse-credentials.json \
        --shell out/credentials/muse-set.txt

安全:
- 値 (host/vm/token) は標準出力・標準エラー・例外メッセージに出さない。
- 出力ファイルは 0600・atomic write。既存ファイルは ``--force`` なしでは上書きしない。
- 指定した NVS/フラッシュ以外は読まない・ネットワークも使わない。
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import tempfile
import zlib
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# ---- NVS 形式の定数 (ESP-IDF と同じ) --------------------------------------

PAGE_SIZE = 4096
ENTRY_SIZE = 32
ENTRIES_PER_PAGE = (PAGE_SIZE - 2 * ENTRY_SIZE) // ENTRY_SIZE  # 126

PAGE_STATUS_ACTIVE = 0xFFFFFFFE
PAGE_STATUS_FULL = 0xFFFFFFFC

ENTRY_STATE_WRITTEN = 0b10

TYPE_U8 = 0x01
TYPE_STRING = 0x21
TYPE_BLOB = 0x41
TYPE_BLOB_DATA = 0x42
TYPE_BLOB_INDEX = 0x48
NUMERIC_TYPES = {0x01, 0x11, 0x02, 0x12, 0x04, 0x14, 0x08, 0x18}

# 公式ファーム (SDK ``partitions_muse.csv``) の NVS パーティション。
DEFAULT_NVS_OFFSET = 0x11000
DEFAULT_NVS_SIZE = 0xC000  # 49,152

# SAS-Watch 側 (muse_link) の制限。SDK の muse_settings.h と同じ。
HOST_MAX = 63
VM_MAX = 63
TOKEN_MAX = 1023
SSID_MAX = 32
PASS_MAX = 63
URL_MAX = 255
DEFAULT_HOST = "hatch.metaaivm.com"
MUSE_NAMESPACE = "muse"
HOMEHUB_NAMESPACE = "homehub"
KEY_HOST = "host"
KEY_VM = "vm"
KEY_VM_LEGACY = "vm_id"  # SAS-Watch Phase 1 の旧キー (読むだけ)
KEY_TOKEN = "token"
KEY_HOMEHUB_TOKEN = "access_token"
KEY_HOMEHUB_TOKEN_LEGACY = "auth_token"
KEY_HOMEHUB_HOST = "noise_host"
KEY_HOMEHUB_VM_URL = "vm_url"
KEY_SSID = "ssid"
KEY_PASS_MUSE = "pass"
KEY_PASS_HOMEHUB = "password"

# SAS-Watch のシリアル行モード (T52) は 1 行 1100 バイトまで。
SERIAL_LINE_MAX = 1100


class NVSError(Exception):
    """NVS の読み出し・解釈に失敗した。メッセージに値は含めないこと。"""


class NVSCorruptError(NVSError):
    """NVS イメージが壊れている。"""


class CredentialsError(NVSError):
    """NVS は読めたが資格情報が欠落・不正。"""


# ---- パース結果 -----------------------------------------------------------


@dataclass
class ParsedValue:
    """NVS の 1 キーの値。kind は 'string' / 'int' / 'other'。"""

    kind: str
    data: object = None


@dataclass
class ParsedNVS:
    """namespace 名 -> キー -> 値。同一キーは新しいページ/エントリで上書き済み。"""

    namespaces: Dict[str, Dict[str, ParsedValue]] = field(default_factory=dict)
    warnings: List[str] = field(default_factory=list)
    pages_seen: int = 0


# ---- NVS パーサ -----------------------------------------------------------


def _decode_key(raw: bytes) -> Optional[str]:
    """NVS のキー (16 バイト、NUL 終端) を ASCII 文字列にする。不正なら None。"""
    key = raw.split(b"\x00", 1)[0]
    if not key:
        return None
    if any(c < 0x20 or c > 0x7E for c in key):
        return None
    try:
        return key.decode("ascii")
    except UnicodeDecodeError:  # pragma: no cover - 上の判定で到達しない
        return None


def _entry_state(bitmap: bytes, index: int) -> int:
    return (bitmap[index // 4] >> ((index % 4) * 2)) & 0b11


def _entry_crc(raw: bytes) -> int:
    # entry: ns, type, span, chunk, crc32, key[16], data[8]
    # CRC は crc32(ns..chunk + key + data) — crc フィールドを除いた 28 バイト。
    return zlib.crc32(raw[:4] + raw[8:32], 0xFFFFFFFF)


def _parse_string_value(
    raw: bytes, span: int, page: bytes, index: int, entry_state
) -> ParsedValue:
    """type=string の値 (先頭エントリ + 後続の生データエントリ) を取り出す。"""
    size = struct.unpack_from("<H", raw, 24)[0]
    data_crc = struct.unpack_from("<I", raw, 28)[0]

    if span < 2:
        raise NVSCorruptError(
            "文字列エントリの span が想定と違います (span=1)。NVS イメージが壊れています"
        )

    chunks = []
    for offset in range(1, span):
        child_index = index + offset
        if entry_state(child_index) != ENTRY_STATE_WRITTEN:
            raise NVSCorruptError(
                "文字列の続きエントリが Written ではありません。NVS イメージが壊れています"
            )
        start = (child_index + 2) * ENTRY_SIZE
        chunks.append(page[start : start + ENTRY_SIZE])
    payload = b"".join(chunks)

    if size > len(payload):
        raise NVSCorruptError(
            "文字列の長さが NVS エントリの範囲を超えています。NVS イメージが壊れています"
        )
    payload = payload[:size]

    if payload and payload[-1] != 0:
        raise NVSCorruptError("文字列が NUL 終端されていません。NVS イメージが壊れています")
    if zlib.crc32(payload, 0xFFFFFFFF) != data_crc:
        raise NVSCorruptError(
            "文字列データの CRC が一致しません。NVS イメージが壊れています "
            "(NVS 暗号化が有効なイメージの可能性もあります)"
        )
    return ParsedValue("string", payload[:-1] if payload else b"")


@dataclass
class _PageResult:
    page_index: int
    page_number: int
    namespaces: List[Tuple[int, str]] = field(default_factory=list)
    values: List[Tuple[int, str, ParsedValue]] = field(default_factory=list)
    skipped_statuses: List[int] = field(default_factory=list)


def _parse_page(page: bytes, page_number: int) -> Optional[_PageResult]:
    """1 ページを解析する。消去済みページなら None。"""
    if page == b"\xff" * PAGE_SIZE:
        return None

    status = struct.unpack_from("<I", page, 0)[0]
    page_index = struct.unpack_from("<I", page, 4)[0]
    version = 256 - page[8]

    if status not in (PAGE_STATUS_ACTIVE, PAGE_STATUS_FULL):
        # 消去中/破損ページ。NVS のウェアレベリングでは正常に起こりうるので
        # 読み飛ばす (警告として記録する)。
        result = _PageResult(page_index, page_number)
        result.skipped_statuses.append(status)
        return result

    if version not in (1, 2):
        raise NVSCorruptError(
            "NVS ページのバージョンが想定外です (version=%d)。NVS イメージが壊れています"
            % version
        )

    header_crc = struct.unpack_from("<I", page, 28)[0]
    if zlib.crc32(page[4:28], 0xFFFFFFFF) != header_crc:
        raise NVSCorruptError(
            "NVS ページヘッダの CRC が一致しません (page_index=%d)。NVS イメージが壊れています"
            % page_index
        )

    result = _PageResult(page_index, page_number)
    bitmap = page[32:64]

    def state(entry_index: int) -> int:
        return _entry_state(bitmap, entry_index)

    index = 0
    while index < ENTRIES_PER_PAGE:
        if state(index) != ENTRY_STATE_WRITTEN:
            index += 1
            continue
        start = (index + 2) * ENTRY_SIZE
        raw = page[start : start + ENTRY_SIZE]
        if len(raw) != ENTRY_SIZE:  # pragma: no cover - ページ長は検証済み
            raise NVSCorruptError("NVS ページが途中で切れています")

        if struct.unpack_from("<I", raw, 4)[0] != _entry_crc(raw):
            raise NVSCorruptError(
                "NVS エントリの CRC が一致しません (page_index=%d, entry=%d)。"
                "NVS イメージが壊れています" % (page_index, index)
            )

        span = raw[2] if raw[2] not in (0, 0xFF) else 1
        if span < 1 or index + span > ENTRIES_PER_PAGE:
            raise NVSCorruptError(
                "NVS エントリの span が範囲外です (page_index=%d, entry=%d, span=%d)。"
                "NVS イメージが壊れています" % (page_index, index, span)
            )

        ns_index = raw[0]
        entry_type = raw[1]
        key = _decode_key(raw[8:24])

        if ns_index == 0:
            # namespace 定義 (type=uint8, data[0]=namespace index)
            if entry_type == TYPE_U8 and key is not None:
                result.namespaces.append((raw[24], key))
        elif key is not None:
            if entry_type == TYPE_STRING:
                value = _parse_string_value(raw, span, page, index, state)
            elif entry_type in NUMERIC_TYPES:
                size = entry_type & 0x0F
                value = ParsedValue("int", int.from_bytes(raw[24 : 24 + size], "little"))
            else:
                value = ParsedValue("other", None)
            result.values.append((ns_index, key, value))

        index += span
    return result


def parse_nvs(data: bytes) -> ParsedNVS:
    """NVS パーティションイメージを解析する。

    壊れたイメージは NVSCorruptError。値は ParsedValue として返す (表示はしない)。
    同じキーが複数ページにある場合は、新しいページ (page_index が大きい方) を採る。
    """
    if not data:
        raise NVSCorruptError("NVS イメージが空です")
    if len(data) % PAGE_SIZE != 0:
        raise NVSCorruptError(
            "NVS イメージのサイズが %d バイトで、ページサイズ (%d) の倍数ではありません"
            % (len(data), PAGE_SIZE)
        )

    page_results: List[_PageResult] = []
    for page_number in range(len(data) // PAGE_SIZE):
        page = data[page_number * PAGE_SIZE : (page_number + 1) * PAGE_SIZE]
        result = _parse_page(page, page_number)
        if result is not None:
            page_results.append(result)

    # NVS は書き込みが進むほど page_index が増える。物理順ではなく page_index 順で見る。
    page_results.sort(key=lambda item: (item.page_index, item.page_number))

    parsed = ParsedNVS(pages_seen=len(data) // PAGE_SIZE)
    ns_by_index: Dict[int, str] = {}
    for result in page_results:
        for status in result.skipped_statuses:
            parsed.warnings.append(
                "ページ (offset 0x%x) の状態が 0x%08x のため読み飛ばしました"
                % (result.page_number * PAGE_SIZE, status)
            )
        for ns_index, name in result.namespaces:
            ns_by_index[ns_index] = name
            parsed.namespaces.setdefault(name, {})

    for result in page_results:
        for ns_index, key, value in result.values:
            name = ns_by_index.get(ns_index)
            if name is None:
                parsed.warnings.append(
                    "namespace index %d の定義が見つからないためキーを無視しました" % ns_index
                )
                continue
            parsed.namespaces[name][key] = value

    if not parsed.namespaces:
        raise NVSCorruptError(
            "NVS イメージに有効な namespace が見つかりません (空/消去済み/壊れている可能性)"
        )
    return parsed


def slice_nvs_from_flash(data: bytes, offset: int, size: int) -> bytes:
    """32MB のフルフラッシュイメージから NVS パーティションを切り出す。"""
    if size <= 0 or size % PAGE_SIZE != 0:
        raise NVSError("NVS サイズは 4096 の倍数で指定してください (指定値: %d バイト)" % size)
    if offset < 0:
        raise NVSError("NVS オフセットに負の値は指定できません")
    if offset + size > len(data):
        # NVS パーティション単体を --flash に渡したときの分かりやすいヒント。
        looks_like_nvs_only = len(data) == size or (
            len(data) % PAGE_SIZE == 0 and len(data) <= 0x100000
        )
        if looks_like_nvs_only:
            raise NVSError(
                "入力は NVS パーティション単体 (%d バイト) に見えます。"
                "NVS 単体には --nvs を、フルフラッシュには --flash を使ってください" % len(data)
            )
        raise NVSError(
            "フルフラッシュイメージが小さすぎます (必要: offset 0x%x + size 0x%x = %d バイト, "
            "実際: %d バイト)。バックアップが途中で切れていないか確認してください"
            % (offset, size, offset + size, len(data))
        )
    return data[offset : offset + size]


# ---- 資格情報の取り出し ---------------------------------------------------


@dataclass
class KeyStatus:
    name: str
    found: bool
    valid: bool = False
    length: int = 0
    error: str = ""
    legacy: bool = False

    @property
    def ok(self) -> bool:
        return self.found and self.valid


@dataclass
class Credentials:
    host: str
    vm: str
    token: str
    vm_legacy: bool = False
    source_namespace: str = MUSE_NAMESPACE


@dataclass
class WifiCredentials:
    ssid: str
    password: str


def _use_homehub(parsed: ParsedNVS) -> bool:
    """muse 側に token が無く homehub namespace があるときは公式ペアリング形式とみなす。"""
    muse_ns = parsed.namespaces.get(MUSE_NAMESPACE)
    homehub_ns = parsed.namespaces.get(HOMEHUB_NAMESPACE)
    muse_has_token = muse_ns is not None and KEY_TOKEN in muse_ns
    return (not muse_has_token) and (homehub_ns is not None)


def _decode_credential(value: ParsedValue, max_len: int) -> Tuple[Optional[str], Optional[str], int]:
    """ParsedValue を検証して文字列にする。

    戻り値: (値 or None, エラー or None, 長さ)。エラー文字列に値は含めない。
    """
    if value.kind != "string":
        return None, "文字列ではありません (NVS の型が違います)", 0
    raw = value.data if isinstance(value.data, bytes) else b""
    if not raw:
        return None, "空です", 0
    if any(byte < 0x21 or byte > 0x7E for byte in raw):
        return None, "空白・制御文字・非 ASCII を含みます", len(raw)
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError:  # pragma: no cover - 上の判定で到達しない
        return None, "ASCII として読めません", len(raw)
    if len(text) > max_len:
        return None, "長すぎます (上限 %d バイト)" % max_len, len(text)
    return text, None, len(text)


def _decode_wifi_str(
    value: ParsedValue, max_len: int, allow_empty: bool
) -> Tuple[Optional[str], Optional[str], int]:
    """Wi-Fi SSID/パスワードを検証して文字列にする (空白 0x20 可、制御文字不可)。"""
    if value.kind != "string":
        return None, "文字列ではありません (NVS の型が違います)", 0
    raw = value.data if isinstance(value.data, bytes) else b""
    if not raw:
        if allow_empty:
            return "", None, 0
        return None, "空です", 0
    if len(raw) > max_len:
        return None, "長すぎます (上限 %d バイト)" % max_len, len(raw)
    if any(byte < 0x20 or byte == 0x7F for byte in raw):
        return None, "改行・制御文字を含みます", len(raw)
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        return None, "UTF-8 として読めません", len(raw)
    return text, None, len(raw)


def _derive_vm_id_from_url(url: str) -> Tuple[Optional[str], Optional[str]]:
    """wss://<vm_id>.<domain>/... から vm_id を取り出す (vm_connect.h の vm_derive_id 互換)。"""
    pos = url.find("://")
    if pos < 0:
        return None, "vm_url から VM ID を取り出せません"
    rest = url[pos + 3 :]
    dot = rest.find(".")
    if dot <= 0:
        return None, "vm_url から VM ID を取り出せません"
    vm_id = rest[:dot]
    if len(vm_id) > VM_MAX:
        return None, "VM ID が長すぎます (上限 %d バイト)" % VM_MAX
    allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-"
    if any(ch not in allowed for ch in vm_id):
        return None, "vm_url の VM ID に不正な文字が含まれます"
    return vm_id, None


def inspect_credentials(parsed: ParsedNVS) -> Dict[str, KeyStatus]:
    """namespace ``muse`` (または公式ペアリング済みの ``homehub``) の host/vm/token の状態を返す (値は含めない)。"""
    statuses: Dict[str, KeyStatus] = {}
    if _use_homehub(parsed):
        ns = parsed.namespaces[HOMEHUB_NAMESPACE]
        host_val = ns.get(KEY_HOMEHUB_HOST)
        if host_val is None or (host_val.kind == "string" and host_val.data == b""):
            statuses[KEY_HOST] = KeyStatus(
                name=KEY_HOST, found=True, valid=True, length=len(DEFAULT_HOST)
            )
        else:
            _text, error, length = _decode_credential(host_val, HOST_MAX)
            statuses[KEY_HOST] = KeyStatus(
                name=KEY_HOST, found=True, valid=error is None, length=length, error=error or ""
            )

        vm_val = ns.get(KEY_HOMEHUB_VM_URL)
        if vm_val is None or (vm_val.kind == "string" and vm_val.data == b""):
            statuses[KEY_VM] = KeyStatus(name=KEY_VM, found=True, valid=True, length=0)
        else:
            url_text, error, length = _decode_credential(vm_val, URL_MAX)
            if error is not None or url_text is None:
                statuses[KEY_VM] = KeyStatus(
                    name=KEY_VM, found=True, valid=False, length=length, error=error or ""
                )
            else:
                vm_id, vm_err = _derive_vm_id_from_url(url_text)
                statuses[KEY_VM] = KeyStatus(
                    name=KEY_VM,
                    found=True,
                    valid=vm_err is None,
                    length=len(vm_id) if vm_id is not None else 0,
                    error=vm_err or "",
                )

        tok_val = ns.get(KEY_HOMEHUB_TOKEN)
        if tok_val is None:
            tok_val = ns.get(KEY_HOMEHUB_TOKEN_LEGACY)
        if tok_val is None:
            statuses[KEY_TOKEN] = KeyStatus(name=KEY_TOKEN, found=False, error="キーがありません")
        else:
            _text, error, length = _decode_credential(tok_val, TOKEN_MAX)
            statuses[KEY_TOKEN] = KeyStatus(
                name=KEY_TOKEN, found=True, valid=error is None, length=length, error=error or ""
            )
        return statuses

    ns = parsed.namespaces.get(MUSE_NAMESPACE)

    if ns is None:
        for key in (KEY_HOST, KEY_VM, KEY_TOKEN):
            statuses[key] = KeyStatus(
                name=key, found=False, error='namespace "muse" がありません'
            )
        return statuses

    for key, max_len in ((KEY_HOST, HOST_MAX), (KEY_VM, VM_MAX), (KEY_TOKEN, TOKEN_MAX)):
        value = ns.get(key)
        legacy = False
        if value is None and key == KEY_VM:
            value = ns.get(KEY_VM_LEGACY)
            legacy = value is not None
        if value is None:
            statuses[key] = KeyStatus(name=key, found=False, error="キーがありません")
            continue
        _text, error, length = _decode_credential(value, max_len)
        statuses[key] = KeyStatus(
            name=key,
            found=True,
            valid=error is None,
            length=length,
            error=error or "",
            legacy=legacy,
        )
    return statuses


def inspect_wifi_credentials(parsed: ParsedNVS) -> Dict[str, KeyStatus]:
    """``homehub`` または ``muse`` から Wi-Fi (ssid/password) の状態を返す (値は含めない)。"""
    homehub_ns = parsed.namespaces.get(HOMEHUB_NAMESPACE)
    muse_ns = parsed.namespaces.get(MUSE_NAMESPACE)

    if homehub_ns is not None and KEY_SSID in homehub_ns:
        ns = homehub_ns
        pass_key = KEY_PASS_HOMEHUB
    elif muse_ns is not None and KEY_SSID in muse_ns:
        ns = muse_ns
        pass_key = KEY_PASS_MUSE
    elif homehub_ns is not None:
        ns = homehub_ns
        pass_key = KEY_PASS_HOMEHUB
    else:
        ns = muse_ns
        pass_key = KEY_PASS_MUSE

    if ns is None:
        return {
            "wifi_ssid": KeyStatus(name="wifi_ssid", found=False, error="namespace がありません"),
            "wifi_pass": KeyStatus(name="wifi_pass", found=False, error="namespace がありません"),
        }

    statuses: Dict[str, KeyStatus] = {}
    ssid_val = ns.get(KEY_SSID)
    if ssid_val is None:
        statuses["wifi_ssid"] = KeyStatus(name="wifi_ssid", found=False, error="キーがありません")
    else:
        _t, err, length = _decode_wifi_str(ssid_val, SSID_MAX, allow_empty=False)
        statuses["wifi_ssid"] = KeyStatus(
            name="wifi_ssid", found=True, valid=err is None, length=length, error=err or ""
        )

    pass_val = ns.get(pass_key)
    if pass_val is None:
        # オープンネットワークでは password キー自体が無い場合がある
        statuses["wifi_pass"] = KeyStatus(name="wifi_pass", found=True, valid=True, length=0)
    else:
        _t, err, length = _decode_wifi_str(pass_val, PASS_MAX, allow_empty=True)
        statuses["wifi_pass"] = KeyStatus(
            name="wifi_pass", found=True, valid=err is None, length=length, error=err or ""
        )
    return statuses


def extract_wifi_credentials(parsed: ParsedNVS) -> WifiCredentials:
    """検証済みの Wi-Fi 資格情報を返す。欠落・不正なら CredentialsError。"""
    statuses = inspect_wifi_credentials(parsed)
    bad = [status for status in statuses.values() if not status.ok]
    if bad:
        details = "、".join(
            "%s: %s" % (status.name, status.error if status.error else "不正") for status in bad
        )
        raise CredentialsError("Wi-Fi の資格情報が揃っていません (%s)" % details)

    homehub_ns = parsed.namespaces.get(HOMEHUB_NAMESPACE)
    muse_ns = parsed.namespaces.get(MUSE_NAMESPACE)
    if homehub_ns is not None and KEY_SSID in homehub_ns:
        ns = homehub_ns
        pass_key = KEY_PASS_HOMEHUB
    else:
        ns = muse_ns  # type: ignore[assignment]
        pass_key = KEY_PASS_MUSE

    ssid_text, err, _len = _decode_wifi_str(ns[KEY_SSID], SSID_MAX, allow_empty=False)
    if ssid_text is None:  # pragma: no cover
        raise CredentialsError("wifi_ssid を読み出せません (%s)" % err)
    pass_val = ns.get(pass_key)
    if pass_val is None:
        pass_text = ""
    else:
        pass_text, err, _len = _decode_wifi_str(pass_val, PASS_MAX, allow_empty=True)
        if pass_text is None:  # pragma: no cover
            raise CredentialsError("wifi_pass を読み出せません (%s)" % err)
    return WifiCredentials(ssid=ssid_text, password=pass_text)


def extract_credentials(parsed: ParsedNVS) -> Credentials:
    """検証済みの資格情報を返す。欠落・不正なら CredentialsError。"""
    statuses = inspect_credentials(parsed)
    bad = [status for status in statuses.values() if not status.ok]
    if bad:
        details = "、".join(
            "%s: %s" % (status.name, status.error if status.error else "不正") for status in bad
        )
        raise CredentialsError("Muse の資格情報が揃っていません (%s)" % details)

    if _use_homehub(parsed):
        ns = parsed.namespaces[HOMEHUB_NAMESPACE]
        host_val = ns.get(KEY_HOMEHUB_HOST)
        if host_val is None or (host_val.kind == "string" and host_val.data == b""):
            host = DEFAULT_HOST
        else:
            host_t, err, _l = _decode_credential(host_val, HOST_MAX)
            if host_t is None:  # pragma: no cover
                raise CredentialsError("host を読み出せません (%s)" % err)
            host = host_t

        vm_val = ns.get(KEY_HOMEHUB_VM_URL)
        if vm_val is None or (vm_val.kind == "string" and vm_val.data == b""):
            vm = ""
        else:
            url_t, err, _l = _decode_credential(vm_val, URL_MAX)
            if url_t is None:  # pragma: no cover
                raise CredentialsError("vm_url を読み出せません (%s)" % err)
            vm_id, vm_err = _derive_vm_id_from_url(url_t)
            if vm_id is None:  # pragma: no cover
                raise CredentialsError("vm_url を読み出せません (%s)" % vm_err)
            vm = vm_id

        tok_val = ns.get(KEY_HOMEHUB_TOKEN) or ns.get(KEY_HOMEHUB_TOKEN_LEGACY)
        if tok_val is None:  # pragma: no cover
            raise CredentialsError("token を読み出せません")
        token_t, err, _l = _decode_credential(tok_val, TOKEN_MAX)
        if token_t is None:  # pragma: no cover
            raise CredentialsError("token を読み出せません (%s)" % err)
        return Credentials(host=host, vm=vm, token=token_t, source_namespace=HOMEHUB_NAMESPACE)

    ns = parsed.namespaces[MUSE_NAMESPACE]

    def read(key: str, max_len: int) -> str:
        text, error, _length = _decode_credential(ns[key], max_len)
        if text is None:  # pragma: no cover - inspect_credentials で検証済み
            raise CredentialsError("%s を読み出せません (%s)" % (key, error))
        return text

    legacy = KEY_VM not in ns
    return Credentials(
        host=read(KEY_HOST, HOST_MAX),
        vm=read(KEY_VM_LEGACY if legacy else KEY_VM, VM_MAX),
        token=read(KEY_TOKEN, TOKEN_MAX),
        vm_legacy=legacy,
        source_namespace=MUSE_NAMESPACE,
    )


# ---- 出力 -----------------------------------------------------------------


def _summary_rows(
    statuses: Dict[str, KeyStatus], keys: Tuple[str, ...] = (KEY_HOST, KEY_VM, KEY_TOKEN)
) -> List[Tuple[str, str]]:
    rows = []
    labels = {
        KEY_HOST: "host ",
        KEY_VM: "vm   ",
        KEY_TOKEN: "token",
        "wifi_ssid": "ssid ",
        "wifi_pass": "pass ",
    }
    for key in keys:
        status = statuses[key]
        if status.ok:
            note = "  (旧キー vm_id から)" if status.legacy else ""
            text = "あり (%d バイト)%s" % (status.length, note)
        elif not status.found:
            text = "なし (%s)" % (status.error or "キーがありません")
        else:
            text = "不正 (%s)" % (status.error or "不明")
        rows.append((labels.get(key, key), text))
    return rows


def print_check(
    source_label: str,
    nvs_size: int,
    parsed: ParsedNVS,
    statuses: Dict[str, KeyStatus],
    wifi_statuses: Optional[Dict[str, KeyStatus]] = None,
) -> bool:
    print("NVS イメージ: %s (%d バイト, %d ページ)" % (source_label, nvs_size, parsed.pages_seen))
    print('namespace "muse": %s' % ("あり" if MUSE_NAMESPACE in parsed.namespaces else "なし"))
    if _use_homehub(parsed):
        print('namespace "homehub": %s' % ("あり" if HOMEHUB_NAMESPACE in parsed.namespaces else "なし"))
    for label, text in _summary_rows(statuses):
        print("  %s: %s" % (label, text))
    all_statuses = list(statuses.values())
    if wifi_statuses is not None:
        for label, text in _summary_rows(wifi_statuses, ("wifi_ssid", "wifi_pass")):
            print("  %s: %s" % (label, text))
        all_statuses.extend(wifi_statuses.values())
    for warning in parsed.warnings:
        print("  警告: %s" % warning)
    good = sum(1 for status in all_statuses if status.ok)
    ok = good == len(all_statuses)
    print("結果: %s (%d/%d)" % ("OK" if ok else "NG", good, len(all_statuses)))
    if not ok:
        print(
            "  token が無い/空なら M1 は NO-GO です (docs/muse-m1.md の GO/NO-GO を参照)。",
            file=sys.stderr,
        )
    return ok


def _atomic_write_0600(path: Path, text: str) -> None:
    """0600 で atomic に書く。途中のファイルが他者に読まれないようにする。"""
    directory = path.parent if str(path.parent) else Path(".")
    directory.mkdir(parents=True, exist_ok=True)
    try:
        os.chmod(directory, 0o700)
    except OSError:
        pass
    fd, tmp_name = tempfile.mkstemp(dir=str(directory), prefix=".tmp-muse-creds-", suffix=".tmp")
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(text)
        os.replace(tmp_name, path)
        os.chmod(path, 0o600)
    except BaseException:
        try:
            os.unlink(tmp_name)
        except OSError:
            pass
        raise


def _check_overwrite(paths: List[Path], force: bool) -> None:
    if force:
        return
    existing = [str(path) for path in paths if path.exists()]
    if existing:
        raise CredentialsError(
            "出力先が既にあります: %s (上書きするなら --force)" % ", ".join(existing)
        )


def write_json(
    path: Path,
    credentials: Credentials,
    source_label: str,
    wifi: Optional[WifiCredentials] = None,
) -> None:
    lengths: Dict[str, int] = {
        "host": len(credentials.host),
        "vm": len(credentials.vm),
        "token": len(credentials.token),
    }
    payload: Dict[str, object] = {
        "host": credentials.host,
        "vm": credentials.vm,
        "token": credentials.token,
        "source_namespace": credentials.source_namespace,
        "lengths": lengths,
        "source": source_label,
        "generated_at": datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds"),
        "warning": "Muse の資格情報。共有・コミットしない。使い終わったら削除する。",
    }
    if wifi is not None:
        payload["wifi_ssid"] = wifi.ssid
        payload["wifi_pass"] = wifi.password
        lengths["wifi_ssid"] = len(wifi.ssid.encode("utf-8"))
        lengths["wifi_pass"] = len(wifi.password.encode("utf-8"))
    _atomic_write_0600(path, json.dumps(payload, ensure_ascii=False, indent=2) + "\n")


def build_shell_lines(
    credentials: Credentials, wifi: Optional[WifiCredentials] = None
) -> List[str]:
    """シリアル行モードに送る行を作る。値はそのまま入るので扱い注意。"""
    lines: List[str] = []
    if wifi is not None:
        lines.append("muse set ssid %s" % wifi.ssid)
        lines.append(("muse set pass %s" % wifi.password) if wifi.password else "muse set pass")
    lines.extend([
        "muse set host %s" % credentials.host,
        ("muse set vm %s" % credentials.vm) if credentials.vm else "muse set vm",
        "muse set token %s" % credentials.token,
        "muse status",
    ])
    for line in lines:
        if len(line.encode("utf-8")) + 1 > SERIAL_LINE_MAX:  # +1 は改行
            raise CredentialsError(
                "シリアル 1 行の上限 (%d バイト) を超えます。値が長すぎます" % SERIAL_LINE_MAX
            )
        if any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in line):
            raise CredentialsError("シリアルに送れない制御文字が含まれています")
    return lines


def write_shell(
    path: Path, credentials: Credentials, wifi: Optional[WifiCredentials] = None
) -> None:
    _atomic_write_0600(path, "\n".join(build_shell_lines(credentials, wifi=wifi)) + "\n")


# ---- CLI ------------------------------------------------------------------


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "公式 Muse Gadget ファームの NVS から host/vm/token を取り出す。"
            "値は画面に出さず、--check ではキーの有無と長さだけを表示する。"
        ),
        formatter_class=argparse.RawTextHelpFormatter,
        epilog=(
            "例:\n"
            "  export_credentials.py --nvs muse-nvs.bin --check\n"
            "  export_credentials.py --flash full-backup.bin --check\n"
            "  export_credentials.py --nvs muse-nvs.bin --include-wifi --out creds.json --shell muse-set.txt\n"
            "\n"
            "NVS の場所 (公式ファーム): offset 0x11000 / size 0xC000 "
            "(--nvs-offset/--nvs-size で変更可)。\n"
            "値は出力ファイル (0600) にだけ書き、標準出力/標準エラー/例外には出さない。"
        ),
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument(
        "--flash",
        metavar="FILE",
        type=Path,
        help="32MB のフルフラッシュイメージ。NVS 部分を切り出して読む",
    )
    source.add_argument(
        "--nvs",
        metavar="FILE",
        type=Path,
        help="NVS パーティション単体のイメージ (48KB など)",
    )
    parser.add_argument(
        "--nvs-offset",
        default=hex(DEFAULT_NVS_OFFSET),
        help="--flash のとき NVS の開始オフセット (既定: 0x11000)",
    )
    parser.add_argument(
        "--nvs-size",
        default=hex(DEFAULT_NVS_SIZE),
        help="--flash のとき NVS のサイズ (既定: 0xC000)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="キーの有無と長さだけを表示して終了 (値は表示しない)。全部揃っていれば exit 0",
    )
    parser.add_argument(
        "--include-wifi",
        action="store_true",
        help="Wi-Fi の ssid/password も検査・書き出し対象に含める (値は画面に出さない)",
    )
    parser.add_argument(
        "--out", metavar="FILE", type=Path, help="資格情報を JSON で書き出す (0600)"
    )
    parser.add_argument(
        "--shell",
        metavar="FILE",
        type=Path,
        help="SAS-Watch のシリアル行モード用コマンド列を書き出す (0600)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="--out/--shell の出力先が既にあっても上書きする",
    )
    return parser


def _parse_hex(value: str, name: str) -> int:
    try:
        number = int(value, 0)
    except ValueError:
        raise NVSError("%s は 16 進数 (例 0x11000) か 10 進数で指定してください" % name) from None
    if number < 0:
        raise NVSError("%s に負の値は指定できません" % name)
    return number


def run(argv: Optional[List[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)

    try:
        if args.flash is not None:
            try:
                data = args.flash.read_bytes()
            except OSError as exc:
                raise NVSError(
                    "フルフラッシュを読めません: %s (%s)" % (args.flash, exc.strerror or exc)
                ) from None
            offset = _parse_hex(args.nvs_offset, "--nvs-offset")
            size = _parse_hex(args.nvs_size, "--nvs-size")
            nvs = slice_nvs_from_flash(data, offset, size)
            source_label = "%s (offset 0x%x, %d バイト)" % (args.flash, offset, size)
        else:
            try:
                nvs = args.nvs.read_bytes()  # type: ignore[union-attr]
            except OSError as exc:
                raise NVSError(
                    "NVS イメージを読めません: %s (%s)" % (args.nvs, exc.strerror or exc)
                ) from None
            source_label = str(args.nvs)

        parsed = parse_nvs(nvs)
        statuses = inspect_credentials(parsed)
        wifi_statuses = inspect_wifi_credentials(parsed) if args.include_wifi else None

        do_check = args.check or (args.out is None and args.shell is None)
        if do_check and not print_check(
            source_label, len(nvs), parsed, statuses, wifi_statuses=wifi_statuses
        ):
            return 1

        if args.out is not None or args.shell is not None:
            credentials = extract_credentials(parsed)
            wifi = extract_wifi_credentials(parsed) if args.include_wifi else None
            outputs: List[Path] = []
            if args.out is not None:
                outputs.append(args.out)
            if args.shell is not None:
                outputs.append(args.shell)
            _check_overwrite(outputs, args.force)
            if args.out is not None:
                write_json(args.out, credentials, source_label, wifi=wifi)
                print("→ JSON を保存しました: %s (0600)" % args.out)
            if args.shell is not None:
                write_shell(args.shell, credentials, wifi=wifi)
                print("→ シリアル投入用コマンド列を保存しました: %s (0600)" % args.shell)
            print(
                "  注意: これらのファイルは Muse の資格情報。共有・コミットしない。"
                "使い終わったら削除する (`shred -u FILE`)"
            )
        return 0
    except NVSError as exc:
        print("エラー: %s" % exc, file=sys.stderr)
        return 1


def main() -> int:
    return run()


if __name__ == "__main__":
    sys.exit(main())
