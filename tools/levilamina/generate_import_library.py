#!/usr/bin/env python3
"""Build and validate a small MSVC import library from a pinned LL DLL.

The export names are read from the PE export directory and copied byte for byte
into a DEF file.  The allowlist is intentionally explicit: ordinal-only,
forwarded, duplicate, and kind-mismatched entries are rejected.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
from shutil import which
from pathlib import Path
from typing import Any


IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_READ = 0x40000000
IMAGE_SCN_MEM_WRITE = 0x80000000


class ImportError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_c_string(data: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(data):
        raise ImportError(f"string RVA points outside the image: 0x{offset:x}")
    end = data.find(b"\0", offset)
    if end < 0:
        raise ImportError(f"unterminated export name at file offset 0x{offset:x}")
    return data[offset:end].decode("ascii")


def parse_pe_exports(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ImportError(f"{path} is not a PE image")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise ImportError(f"{path} has no PE signature")

    file_header = pe_offset + 4
    machine, section_count, _, _, _, optional_size, _ = struct.unpack_from("<HHIIIHH", data, file_header)
    if machine != IMAGE_FILE_MACHINE_AMD64:
        raise ImportError(f"{path} is not AMD64 (machine 0x{machine:04x})")
    optional = file_header + 20
    magic = struct.unpack_from("<H", data, optional)[0]
    if magic != 0x20B:
        raise ImportError(f"{path} is not a PE32+ image")
    export_rva, export_size = struct.unpack_from("<II", data, optional + 112)

    sections: list[dict[str, int]] = []
    section_table = optional + optional_size
    for index in range(section_count):
        at = section_table + index * 40
        name = data[at : at + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", data, at + 8)
        characteristics = struct.unpack_from("<I", data, at + 36)[0]
        sections.append(
            {
                "name": name,
                "rva": virtual_address,
                "size": max(virtual_size, raw_size),
                "raw": raw_pointer,
                "raw_size": raw_size,
                "characteristics": characteristics,
            }
        )

    def rva_to_file(rva: int) -> int:
        for section in sections:
            if section["rva"] <= rva < section["rva"] + section["size"]:
                delta = rva - section["rva"]
                if delta >= section["raw_size"]:
                    raise ImportError(f"RVA 0x{rva:x} has no raw file backing")
                return section["raw"] + delta
        raise ImportError(f"RVA 0x{rva:x} is outside all sections")

    export_offset = rva_to_file(export_rva)
    (
        _characteristics,
        _timestamp,
        _major,
        _minor,
        _name_rva,
        ordinal_base,
        function_count,
        name_count,
        address_functions,
        address_names,
        address_ordinals,
    ) = struct.unpack_from("<IIHHIIIIIII", data, export_offset)

    def section_for_rva(rva: int) -> dict[str, int] | None:
        return next(
            (section for section in sections if section["rva"] <= rva < section["rva"] + section["size"]),
            None,
        )

    records: list[dict[str, Any]] = []
    names_by_ordinal: dict[int, list[str]] = {}
    for index in range(name_count):
        name_rva = struct.unpack_from("<I", data, rva_to_file(address_names) + index * 4)[0]
        name = read_c_string(data, rva_to_file(name_rva))
        ordinal_index = struct.unpack_from("<H", data, rva_to_file(address_ordinals) + index * 2)[0]
        if ordinal_index >= function_count:
            raise ImportError(f"export {name!r} has an invalid function index {ordinal_index}")
        ordinal = ordinal_base + ordinal_index
        names_by_ordinal.setdefault(ordinal, []).append(name)

    for ordinal_index in range(function_count):
        ordinal = ordinal_base + ordinal_index
        function_rva = struct.unpack_from("<I", data, rva_to_file(address_functions) + ordinal_index * 4)[0]
        forwarder = None
        if export_rva <= function_rva < export_rva + export_size:
            forwarder = read_c_string(data, rva_to_file(function_rva))
            kind = "forwarder"
        else:
            section = section_for_rva(function_rva)
            if section is None:
                kind = "unknown"
            elif section["characteristics"] & IMAGE_SCN_MEM_EXECUTE:
                kind = "code"
            elif section["characteristics"] & (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE):
                kind = "data"
            else:
                kind = "unknown"
        names = names_by_ordinal.get(ordinal, [None])
        for name in names:
            records.append(
                {
                    "name": name,
                    "ordinal": ordinal,
                    "rva": function_rva,
                    "kind": kind,
                    "forwarder": forwarder,
                }
            )

    return {
        "path": str(path),
        "basename": path.name,
        "machine": machine,
        "sha256": sha256(path),
        "export_rva": export_rva,
        "export_size": export_size,
        "sections": sections,
        "records": records,
    }


def read_allowlist(path: Path) -> list[dict[str, Any]]:
    value = json.loads(path.read_text(encoding="utf-8"))
    raw = value.get("symbols") if isinstance(value, dict) else value
    if not isinstance(raw, list) or not raw:
        raise ImportError(f"{path} must contain a non-empty symbols array")
    result = []
    for item in raw:
        if isinstance(item, str):
            result.append({"name": item})
        elif isinstance(item, dict) and isinstance(item.get("name"), str):
            result.append(dict(item))
        else:
            raise ImportError(f"invalid allowlist entry: {item!r}")
    return result


def admit_exports(inventory: dict[str, Any], requested: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_name: dict[str, list[dict[str, Any]]] = {}
    for record in inventory["records"]:
        if record["name"] is not None:
            by_name.setdefault(record["name"], []).append(record)
    admitted = []
    for item in requested:
        name = item["name"]
        matches = by_name.get(name, [])
        if len(matches) != 1:
            raise ImportError(f"required named export {name!r} is missing or ambiguous")
        record = matches[0]
        if record["forwarder"] is not None or record["kind"] == "forwarder":
            raise ImportError(f"required export {name!r} is a forwarder ({record['forwarder']!r})")
        if record["kind"] not in {"code", "data"}:
            raise ImportError(f"required export {name!r} has unsupported kind {record['kind']!r}")
        expected_kind = item.get("kind")
        if expected_kind is not None and expected_kind != record["kind"]:
            raise ImportError(
                f"required export {name!r} kind mismatch: expected {expected_kind!r}, got {record['kind']!r}"
            )
        admitted.append(record)
    return admitted


def write_def(path: Path, dll_name: str, admitted: list[dict[str, Any]]) -> None:
    lines = [f"LIBRARY {dll_name}", "EXPORTS"]
    for record in admitted:
        suffix = " DATA" if record["kind"] == "data" else ""
        lines.append(f"    {record['name']} @{record['ordinal']}{suffix}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")


def run_import_tool(def_path: Path, lib_path: Path, tool: Path | None, dlltool: Path | None) -> list[str]:
    lib_path.parent.mkdir(parents=True, exist_ok=True)
    if tool is not None:
        command = [str(tool), "/nologo", "/machine:x64", f"/def:{def_path}", f"/out:{lib_path}"]
    elif dlltool is not None:
        command = [str(dlltool), "--machine", "i386:x86-64", "--input-def", str(def_path), "--output-lib", str(lib_path)]
    else:
        raise ImportError("no MSVC lib.exe or llvm-dlltool was provided")
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise ImportError(f"import-library tool failed ({completed.returncode}): {completed.stdout}{completed.stderr}")
    if not lib_path.is_file() or lib_path.stat().st_size == 0:
        raise ImportError(f"import-library tool did not create {lib_path}")
    return command


def parse_short_import_members(lib_path: Path) -> list[dict[str, Any]]:
    data = lib_path.read_bytes()
    if not data.startswith(b"!<arch>\n"):
        raise ImportError(f"{lib_path} is not a COFF archive")
    members: list[dict[str, Any]] = []
    offset = 8
    while offset < len(data):
        if offset + 60 > len(data):
            raise ImportError(f"truncated archive member header at 0x{offset:x}")
        header = data[offset : offset + 60]
        offset += 60
        try:
            size = int(header[48:58].decode("ascii").strip())
        except ValueError as error:
            raise ImportError(f"invalid archive member size at 0x{offset - 60:x}") from error
        if offset + size > len(data):
            raise ImportError(f"truncated archive member at 0x{offset:x}")
        member = data[offset : offset + size]
        offset += size + (size & 1)
        if len(member) < 20 or member[:4] != b"\0\0\xff\xff":
            continue
        signature, signature2, version, machine, timestamp, size_of_data, hint, type_name = struct.unpack_from(
            "<HHHHIIHH", member, 0
        )
        if signature != 0 or signature2 != 0xFFFF or version != 0:
            raise ImportError("invalid COFF short-import signature")
        payload = member[20 : 20 + size_of_data]
        parts = payload.split(b"\0")
        if len(parts) < 2 or not parts[0] or not parts[1]:
            raise ImportError("COFF short-import member has no symbol/DLL name pair")
        try:
            name = parts[0].decode("ascii")
            dll_name = parts[1].decode("ascii")
        except UnicodeDecodeError as error:
            raise ImportError("COFF short-import names are not ASCII") from error
        members.append(
            {
                "name": name,
                "dll": dll_name,
                "machine": machine,
                "ordinal_hint": hint,
                "kind": ("code", "data", "const")[type_name & 0x3] if (type_name & 0x3) < 3 else "unknown",
                "name_type": type_name >> 2,
            }
        )
    if not members:
        raise ImportError(f"{lib_path} has no COFF short-import members")
    return members


def validate_short_import_library(lib_path: Path, dll_name: str, admitted: list[dict[str, Any]]) -> dict[str, Any]:
    members = parse_short_import_members(lib_path)
    expected = {record["name"]: record["kind"] for record in admitted}
    actual: dict[str, str] = {}
    for member in members:
        if member["machine"] != IMAGE_FILE_MACHINE_AMD64:
            raise ImportError(f"COFF short-import {member['name']!r} is not AMD64")
        if member["dll"] != dll_name:
            raise ImportError(f"COFF short-import {member['name']!r} binds {member['dll']!r}, expected {dll_name!r}")
        if member["name_type"] != 1:
            raise ImportError(f"COFF short-import {member['name']!r} uses unsupported name mode {member['name_type']}")
        if member["name"] in actual:
            raise ImportError(f"COFF short-import contains duplicate {member['name']!r}")
        actual[member["name"]] = member["kind"]
    if actual != expected:
        raise ImportError(f"COFF short-import inventory mismatch: expected {len(expected)}, got {len(actual)}")
    return {
        "machine": "AMD64",
        "dll": dll_name,
        "member_count": len(members),
        "names": [member["name"] for member in members],
        "name_modes": sorted({member["name_type"] for member in members}),
        "kinds": {name: actual[name] for name in sorted(actual)},
    }


def generate(args: argparse.Namespace) -> int:
    dll = Path(args.dll).resolve()
    pdb = Path(args.pdb).resolve() if args.pdb else None
    allowlist_path = Path(args.allowlist).resolve()
    inventory = parse_pe_exports(dll)
    allowlist_value = json.loads(allowlist_path.read_text(encoding="utf-8"))
    expected_dll_sha256 = allowlist_value.get("runtime_dll_sha256") if isinstance(allowlist_value, dict) else None
    if expected_dll_sha256 is not None and expected_dll_sha256 != inventory["sha256"]:
        raise ImportError(
            f"runtime DLL hash mismatch: allowlist expects {expected_dll_sha256}, actual {inventory['sha256']}"
        )
    requested = read_allowlist(allowlist_path)
    admitted = admit_exports(inventory, requested)
    if pdb is not None and not pdb.is_file():
        raise ImportError(f"PDB does not exist: {pdb}")

    def_path = Path(args.output_def).resolve()
    lib_path = Path(args.output_lib).resolve()
    write_def(def_path, inventory["basename"], admitted)
    tool = Path(args.lib_tool).resolve() if args.lib_tool else None
    dlltool = Path(args.dlltool).resolve() if args.dlltool else None
    command = run_import_tool(def_path, lib_path, tool, dlltool)
    coff_validation = validate_short_import_library(lib_path, inventory["basename"], admitted)
    receipt = {
        "schema_version": 1,
        "dll": inventory["basename"],
        "dll_sha256": inventory["sha256"],
        "pdb": str(pdb) if pdb else None,
        "pdb_sha256": sha256(pdb) if pdb else None,
        "machine": "AMD64",
        "export_count": len(inventory["records"]),
        "admitted": admitted,
        "omitted_ordinal_only": sum(1 for record in inventory["records"] if record["name"] is None),
        "allowlist": str(allowlist_path),
        "def": str(def_path),
        "library": str(lib_path),
        "command": command,
        "coff_validation": coff_validation,
    }
    receipt_path = Path(args.receipt).resolve()
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return 0


def self_test() -> int:
    inventory = {
        "records": [
            {"name": "?decorated@@YAXXZ", "ordinal": 1, "rva": 0x1000, "kind": "code", "forwarder": None},
            {"name": "global_data", "ordinal": 2, "rva": 0x2000, "kind": "data", "forwarder": None},
            {"name": "forwarded", "ordinal": 3, "rva": 0x3000, "kind": "forwarder", "forwarder": "OTHER.dll.fn"},
            {"name": None, "ordinal": 4, "rva": 0x4000, "kind": "code", "forwarder": None},
        ]
    }
    admitted = admit_exports(
        inventory,
        [{"name": "?decorated@@YAXXZ", "kind": "code"}, {"name": "global_data", "kind": "data"}],
    )
    if [item["name"] for item in admitted] != ["?decorated@@YAXXZ", "global_data"]:
        raise ImportError("synthetic decorated/data admission failed")
    for rejected in ([{"name": "forwarded"}], [{"name": "missing"}], [{"name": "global_data", "kind": "code"}]):
        try:
            admit_exports(inventory, rejected)
        except ImportError:
            continue
        raise ImportError(f"synthetic rejection failed for {rejected!r}")

    # Exercise the complete PE export -> DEF -> COFF short-import path with a
    # tiny compiler-generated DLL containing one decorated code and data export.
    clang = which("clang-cl")
    lld = which("lld-link")
    dlltool = which("llvm-dlltool")
    if not clang or not lld or not dlltool:
        raise ImportError("synthetic PE fixture requires clang-cl, lld-link, and llvm-dlltool")
    with tempfile.TemporaryDirectory(prefix="spark-ll-import-fixture-") as raw_dir:
        directory = Path(raw_dir)
        source = directory / "fixture.cpp"
        obj = directory / "fixture.obj"
        dll = directory / "fixture.dll"
        definition = directory / "fixture.def"
        library = directory / "fixture.lib"
        source.write_text("__declspec(dllexport) void fixture_code() {}\n__declspec(dllexport) int fixture_data = 7;\n", encoding="ascii")
        compiled = subprocess.run(
            [clang, "/nologo", "/c", "/EHsc", "/GS-", "/Zl", str(source), f"/Fo{obj}"],
            capture_output=True,
            text=True,
            check=False,
        )
        if compiled.returncode != 0:
            raise ImportError(f"synthetic PE fixture compile failed: {compiled.stdout}{compiled.stderr}")
        linked = subprocess.run(
            [lld, "/dll", "/noentry", "/machine:x64", f"/out:{dll}", str(obj)],
            capture_output=True,
            text=True,
            check=False,
        )
        if linked.returncode != 0:
            raise ImportError(f"synthetic PE fixture link failed: {linked.stdout}{linked.stderr}")
        fixture_inventory = parse_pe_exports(dll)
        by_kind = {kind: [record for record in fixture_inventory["records"] if record["name"] and record["kind"] == kind] for kind in ("code", "data")}
        if not by_kind["code"] or not by_kind["data"]:
            raise ImportError("synthetic PE fixture did not produce both code and data exports")
        fixture_requested = [
            {"name": by_kind["code"][0]["name"], "kind": "code"},
            {"name": by_kind["data"][0]["name"], "kind": "data"},
        ]
        fixture_admitted = admit_exports(fixture_inventory, fixture_requested)
        write_def(definition, fixture_inventory["basename"], fixture_admitted)
        run_import_tool(definition, library, None, Path(dlltool))
        validate_short_import_library(library, fixture_inventory["basename"], fixture_admitted)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--dll")
    parser.add_argument("--pdb")
    parser.add_argument("--allowlist")
    parser.add_argument("--output-def")
    parser.add_argument("--output-lib")
    parser.add_argument("--receipt")
    parser.add_argument("--lib-tool")
    parser.add_argument("--dlltool")
    args = parser.parse_args()
    try:
        if args.self_test:
            return self_test()
        required = ("dll", "allowlist", "output-def", "output-lib", "receipt")
        missing = [name for name in required if not getattr(args, name.replace("-", "_"))]
        if missing:
            parser.error("missing required arguments: " + ", ".join(missing))
        return generate(args)
    except ImportError as error:
        print(f"generate_import_library: error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
