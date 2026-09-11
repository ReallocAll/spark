import pathlib
import json
import re
import struct
import subprocess
import sys
import tarfile
import tempfile


EXPECTED = {"endstone_spark.so"}


def check(condition, message):
    if not condition:
        raise ValueError(message)


def verify_elf(path):
    data = path.read_bytes()
    check(data[:6] == b"\x7fELF\x02\x01", "plugin must be ELF64 little endian")
    check(struct.unpack_from("<H", data, 18)[0] == 62, "plugin must be x86-64")
    offset = struct.unpack_from("<Q", data, 32)[0]
    size, count = struct.unpack_from("<HH", data, 54)
    for index in range(count):
        kind, flags, *_ = struct.unpack_from("<IIQQQQQQ", data, offset + index * size)
        check(kind != 1 or flags & 3 != 3, "plugin has writable executable memory")
    dynamic = subprocess.check_output(["readelf", "-d", str(path)], text=True)
    check("libspark_allocation_gateway" not in dynamic, "plugin depends on a helper ELF")
    symbols = subprocess.check_output(["readelf", "-sW", str(path)], text=True)
    required = {"_U_dyn_register", "_U_dyn_cancel", "_U_dyn_info_list_addr", "_U_dyn_info_list",
                "_ULx86_64_step", "_ULx86_64_get_proc_info_by_ip"}
    for name in required:
        definitions = [line.split() for line in symbols.splitlines() if line.split() and line.split()[-1] == name]
        check(len(definitions) == 1 and definitions[0][4] == "LOCAL" and definitions[0][6] != "UND",
              f"private GNU libunwind symbol is not local: {name}")
    exports = subprocess.check_output(["nm", "-D", "--defined-only", str(path)], text=True)
    check("spark_allocation_gateway_v1" not in exports, "internal gateway API must not be exported")
    print("ELF PASS: no helper dependency, no RWX segment, private GNU libunwind localized")


def verify_stage(path):
    check(path.is_dir() and not path.is_symlink(), "invalid runtime staging root")
    entries = list(path.rglob("*"))
    check(all(not entry.is_symlink() for entry in entries), "symlink in runtime stage")
    files = {entry.relative_to(path).as_posix() for entry in entries if entry.is_file()}
    check(files == EXPECTED, f"unexpected runtime files: {files}")
    check(not any(entry.is_dir() for entry in entries),
          "unexpected runtime directories")


def verify_arena(path):
    code = (path / "code.bin").read_bytes()
    metadata = (path / "metadata.bin").read_bytes()
    manifest = json.loads((path / "manifest.json").read_text())
    count, stride, cie_size, fde_size = 2048, 512, 32, 64
    check(len(code) == count * stride, "incorrect production code arena size")
    check(manifest["metadata"] == manifest["code"] + len(code), "noncontiguous signed-offset arena")
    check(metadata[8:24] == b"\x01zR\0\x01\x78\x10\x01\0\x0c\x07\x08\x90\x01\x08\x06",
          "CIE must use RSP+8, saved return PC, same-value RBP, and no personality")
    listing = subprocess.check_output([
        "objdump", "-D", "-b", "binary", "-m", "i386:x86-64", "-Mintel", "--insn-width=16",
        "--start-address=0", "--stop-address=4096", str(path / "code.bin")], text=True)
    instructions = [[] for _ in range(8)]
    for line in listing.splitlines():
        match = re.match(r"\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}\s)+)\s*(.*?)\s*$", line)
        if match:
            address = int(match[1], 16)
            instructions[address // stride].append((address % stride, len(match[2].split()), match[3]))
    templates = []
    for api, decoded in enumerate(instructions):
        check(decoded and decoded[0][2] == "endbr64", "gateway lacks ENDBR64")
        live = [(offset, size, text) for offset, size, text in decoded if text != "int3"]
        allowed = {"endbr64", "movabs", "mov", "test", "js", "cmp", "jae", "lea", "lock", "je", "dec",
                   "jne", "ret", "jmp", "push", "pop", "call"}
        check(all(text.split()[0] in allowed for _, _, text in live), "unexpected production gateway instruction")
        normalized = [text.replace(" ", "").replace("\t", "") for _, _, text in live]
        check(sum(text.startswith("lockcmpxchg") for text in normalized) == 1, "admission must use one CAS site")
        check("movr9d,0x4" in normalized and "decr9d" in normalized and "movr10d,0xffffffff" in normalized,
              "admission attempts or count saturation are not bounded")
        check(normalized.count("callrax") == 1 and normalized.count("pushrbp") == 1 and
              normalized.count("poprbp") == 1 and normalized.count("movrbp,rsp") == 1,
              "incorrect callback ABI frame")
        check(sum(text.startswith("locksub") for text in normalized) == 1, "missing permanent admission decrement")
        check(not any(re.search(r"r(?:bx|12|13|14|15)", text) for text in normalized), "callee-saved register modified")
        check(not any("fs:" in text or "gs:" in text or "[rsp" in text or "[rbp" in text for text in normalized),
              "gateway uses TLS or red-zone storage")
        for _, _, text in live:
            fields = text.split()
            if fields[0] in {"mov", "movabs", "lea"}:
                destination = fields[1].split(",")[0]
                check(destination in {"rax", "r9d", "r10d", "r10", "r11", "r8", "rdi", "rsi", "rdx", "rcx", "rbp"},
                      "gateway writes an unsupported register or stack location")
                check(destination != "rbp" or text.replace(" ", "") == "movrbp,rsp", "unsupported frame-pointer write")
        check(sum(text == "jmpr8" for text in normalized) == (api != 7), "incorrect original tail jump")
        pointers = [offset + 2 for offset, _, text in live if text.replace(" ", "").startswith("movabsr11,")]
        check(len(pointers) == 2, "gateway embeds an unexpected absolute address")
        push_end = next(offset + size for offset, size, text in live if text.replace(" ", "") == "pushrbp")
        frame_end = next(offset + size for offset, size, text in live if text.replace(" ", "") == "movrbp,rsp")
        pop_end = next(offset + size for offset, size, text in live if text.replace(" ", "") == "poprbp")
        templates.append((code[api * stride:(api + 1) * stride], pointers, push_end, frame_end, pop_end))
    for index in range(count):
        template, pointers, push_end, frame_end, pop_end = templates[index % 8]
        expected = bytearray(template)
        state = (manifest["state"] + manifest["groups_offset"] + index // 8 * manifest["group_stride"] +
                 index % 8 * manifest["entry_stride"])
        for pointer in pointers:
            struct.pack_into("<Q", expected, pointer, state)
        check(code[index * stride:(index + 1) * stride] == expected, "gateway contains nonpermanent embedded state")
        offset = cie_size + index * fde_size
        length, cie_delta, start, extent = struct.unpack_from("<IIQQ", metadata, offset)
        check(length == fde_size - 4 and cie_delta == offset + 4 and
              start == manifest["code"] + index * stride and extent == stride and metadata[offset + 24] == 0,
              "incorrect FDE header or augmentation")
        rows, location, current = {}, 0, (7, 8, None)
        cursor = offset + 25
        while cursor < offset + fde_size:
            opcode = metadata[cursor]
            cursor += 1
            if opcode == 0:
                continue
            if opcode == 3:
                rows[location] = current
                location += struct.unpack_from("<H", metadata, cursor)[0]
                cursor += 2
            elif opcode == 0x0c:
                current = (metadata[cursor], metadata[cursor + 1], current[2])
                cursor += 2
            elif opcode == 0x0d:
                current = (metadata[cursor], current[1], current[2])
                cursor += 1
            elif opcode == 0x0e:
                current = (current[0], metadata[cursor], current[2])
                cursor += 1
            elif opcode == 0x86:
                current = (current[0], current[1], -8 * metadata[cursor])
                cursor += 1
            elif opcode == 0xc6:
                current = (current[0], current[1], None)
            else:
                raise ValueError(f"unsupported gateway CFI opcode {opcode}")
        rows[location] = current
        for pc in range(stride):
            expected_row = ((7, 8, None) if pc < push_end or pc >= pop_end else
                            (7, 16, -16) if pc < frame_end else (6, 16, -16))
            check(rows[max(address for address in rows if address <= pc)] == expected_row,
                  f"CFI disagrees with decoded instructions at entry {index}, offset {pc}")
        table = cie_size + count * fde_size + 8 + index * 8
        check(struct.unpack_from("<ii", metadata, table) == (index * stride, len(code) + offset),
              "private GNU unwind table is not sorted signed offsets from segment base")
    check(metadata[cie_size + count * fde_size:cie_size + count * fde_size + 8] == bytes(8), "missing frame terminator")
    print("Arena PASS: 2048 production stubs, bounded CAS, permanent addresses, ABI frame, every-byte CFI, sorted GNU table")


def verify_provider(path):
    dynamic = subprocess.check_output(["readelf", "-dW", str(path)], text=True)
    needed = [line for line in dynamic.splitlines() if "(NEEDED)" in line]
    check(all("endstone_spark" not in line for line in needed), "provider unexpectedly declares Spark DT_NEEDED")
    symbols = subprocess.check_output(["readelf", "--dyn-syms", "-W", str(path)], text=True)
    if "unlinked" in path.name:
        check(any(" UND " in line and "spark_gateway_unlinked_import" in line for line in symbols.splitlines()),
              "unlinked provider must have an actual undefined Spark import")
    else:
        check(any(" UND " in line and "dlsym" in line for line in symbols.splitlines()),
              "lookup provider must import dlsym")
    print("PASS: provider ELF has no Spark DT_NEEDED and contains the intended unresolved binding")


def verify_archive(path):
    with tarfile.open(path, "r:gz") as archive:
        members = archive.getmembers()
        check(len(members) == 1, "runtime archive must contain exactly one member")
        check({member.name for member in members} == EXPECTED, "unexpected runtime archive members")
        for member in members:
            check(member.isfile() and not member.issym() and not member.islnk(),
                  "runtime members must be regular files")
            name = pathlib.PurePosixPath(member.name)
            check(not name.is_absolute() and ".." not in name.parts and ".local" not in name.parts,
                  "unsafe runtime archive member path")
    print("Linux runtime archive PASS: exactly one regular file")


def exercise_archive(cmake, build, archive, executable):
    subprocess.run([cmake, "--build", build, "--target", "spark_linux_archive"], check=True)
    verify_archive(pathlib.Path(archive))
    with tempfile.TemporaryDirectory(prefix="spark-gateway-extracted-") as directory:
        with tarfile.open(archive, "r:gz") as bundle:
            bundle.extractall(directory, filter="data")
        verify_stage(pathlib.Path(directory))
        subprocess.run([executable, "installed", directory], check=True)


def reject_unsafe_archives():
    variants = [
        ["/endstone_spark.so"],
        ["../endstone_spark.so"],
        ["endstone_spark.so", ".local/libspark_allocation_gateway_v1.so"],
        ["endstone_spark.so", ".spark-native/libspark_allocation_gateway_v1.so", "extra"],
        ["endstone_spark.so", "endstone_spark.so"],
    ]
    with tempfile.TemporaryDirectory(prefix="spark-gateway-invalid-") as directory:
        path = pathlib.Path(directory) / "invalid.tar.gz"
        for names in variants:
            with tarfile.open(path, "w:gz") as archive:
                for name in names:
                    archive.addfile(tarfile.TarInfo(name))
            try:
                verify_archive(path)
            except ValueError:
                continue
            raise ValueError(f"unsafe archive accepted: {names}")
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.DIRTYPE):
            with tarfile.open(path, "w:gz") as archive:
                member = tarfile.TarInfo("endstone_spark.so")
                member.type = kind
                member.linkname = "endstone_spark.so"
                archive.addfile(member)
            try:
                verify_archive(path)
            except ValueError:
                continue
            raise ValueError(f"non-regular archive member accepted: {kind}")
    print("Linux archive rejection PASS: absolute/parent/.local/extra/duplicate/symlink/hardlink/directory")


if __name__ == "__main__":
    if sys.argv[1] == "exercise":
        exercise_archive(*sys.argv[2:])
    elif sys.argv[1] == "negative":
        reject_unsafe_archives()
    else:
        {"elf": verify_elf, "stage": verify_stage, "archive": verify_archive, "arena": verify_arena,
         "provider": verify_provider}[sys.argv[1]](pathlib.Path(sys.argv[2]))
