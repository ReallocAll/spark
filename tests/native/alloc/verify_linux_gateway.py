import pathlib
import struct
import subprocess
import sys
import tarfile
import tempfile


EXPECTED = {"endstone_spark.so", ".spark-native/libspark_allocation_gateway_v1.so"}


def check(condition, message):
    if not condition:
        raise ValueError(message)


def verify_elf(path):
    data = path.read_bytes()
    check(data[:6] == b"\x7fELF\x02\x01", "helper must be ELF64 little endian")
    check(struct.unpack_from("<H", data, 18)[0] == 62, "helper must be x86-64")
    offset = struct.unpack_from("<Q", data, 32)[0]
    size, count = struct.unpack_from("<HH", data, 54)
    rx = rw = 0
    relro = False
    for index in range(count):
        kind, flags, _, _, _, _, memory, _ = struct.unpack_from("<IIQQQQQQ", data, offset + index * size)
        check(kind != 7, "helper must not have TLS")
        relro |= kind == 0x6474E552
        if kind != 1:
            continue
        check(flags & 3 != 3, "helper has writable executable memory")
        rx += memory if flags & 1 else 0
        rw += memory if flags & 2 else 0
    check(rx <= 1024 * 1024 and rw <= 1024 * 1024, f"helper exceeds memory budget: RX={rx}, RW={rw}")
    dynamic = subprocess.check_output(["readelf", "-d", str(path)], text=True)
    check("[libspark_allocation_gateway_v1.so]" in dynamic, "incorrect helper SONAME")
    check("NODELETE" not in dynamic, "plain dlopen must not retain the helper")
    check("BIND_NOW" in dynamic and relro, "helper requires eager binding and RELRO")
    for tag in ("(INIT)", "(FINI)", "(INIT_ARRAY)", "(FINI_ARRAY)", "(PREINIT_ARRAY)",
                "(FILTER)", "(AUXILIARY)", "(AUDIT)", "(DEPAUDIT)", "(TEXTREL)", "(REL)", "(RELR)"):
        check(tag not in dynamic, f"helper has unsupported loader behavior: {tag}")
    check("endstone_spark" not in dynamic and "cpptrace" not in dynamic, "helper has a forbidden dependency")
    exports = subprocess.check_output(["nm", "-D", "--defined-only", str(path)], text=True).splitlines()
    check({line.split()[-1] for line in exports} == {"spark_allocation_gateway_v1"}, f"unexpected exports: {exports}")
    sections = subprocess.check_output(["readelf", "-SW", str(path)], text=True)
    check(".eh_frame" in sections, "missing helper unwind information")
    relocations = subprocess.check_output(["readelf", "-rW", str(path)], text=True)
    allowed = {"R_X86_64_NONE", "R_X86_64_RELATIVE", "R_X86_64_64", "R_X86_64_GLOB_DAT", "R_X86_64_JUMP_SLOT"}
    for line in relocations.splitlines():
        for field in line.split():
            if field.startswith("R_X86_64_"):
                check(field in allowed, f"unsupported helper relocation: {field}")
    symbols = subprocess.check_output(["readelf", "--dyn-syms", "-W", str(path)], text=True)
    check("IFUNC" not in symbols and "UNIQUE" not in symbols and " TLS " not in symbols,
          "helper has unsupported dynamic symbols")
    print(f"ELF gateway PASS: RX={rx}, RW={rw}, sole ABI export, unwind section, no NODELETE")


def verify_stage(path):
    check(path.is_dir() and not path.is_symlink(), "invalid runtime staging root")
    entries = list(path.rglob("*"))
    check(all(not entry.is_symlink() for entry in entries), "symlink in runtime stage")
    files = {entry.relative_to(path).as_posix() for entry in entries if entry.is_file()}
    check(files == EXPECTED, f"unexpected runtime files: {files}")
    check({entry.relative_to(path).as_posix() for entry in entries if entry.is_dir()} == {".spark-native"},
          "unexpected runtime directories")


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
        check(len(members) == 2, "runtime archive must contain exactly two members")
        check({member.name for member in members} == EXPECTED, "unexpected runtime archive members")
        for member in members:
            check(member.isfile() and not member.issym() and not member.islnk(),
                  "runtime members must be regular files")
            name = pathlib.PurePosixPath(member.name)
            check(not name.is_absolute() and ".." not in name.parts and ".local" not in name.parts,
                  "unsafe runtime archive member path")
    print("Linux runtime archive PASS: exactly two regular files")


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
        ["/endstone_spark.so", ".spark-native/libspark_allocation_gateway_v1.so"],
        ["../endstone_spark.so", ".spark-native/libspark_allocation_gateway_v1.so"],
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
                archive.addfile(tarfile.TarInfo("endstone_spark.so"))
                member = tarfile.TarInfo(".spark-native/libspark_allocation_gateway_v1.so")
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
        {"elf": verify_elf, "stage": verify_stage, "archive": verify_archive,
         "provider": verify_provider}[sys.argv[1]](pathlib.Path(sys.argv[2]))
