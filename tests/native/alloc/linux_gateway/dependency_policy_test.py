import pathlib
import struct
import subprocess
import sys
import tempfile


def verify(parser, source, directory):
    data = pathlib.Path(source).read_bytes()
    phoff = struct.unpack_from("<Q", data, 32)[0]
    size, count = struct.unpack_from("<HH", data, 54)
    headers = [struct.unpack_from("<IIQQQQQQ", data, phoff + size * index) for index in range(count)]
    if not any(header[0] == 7 for header in headers):
        raise ValueError("positive dependency fixture must contain PT_TLS")
    dynamic = next(header for header in headers if header[0] == 2)
    tags = {}
    for position in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        tag, value = struct.unpack_from("<qQ", data, position)
        if tag == 0:
            break
        tags[tag] = position
    if 12 not in tags or 25 not in tags:
        raise ValueError("positive dependency fixture must contain ordinary constructor metadata")
    subprocess.run([parser, source, "accept", "dependency"], check=True)
    for name, tag in {"filter": 0x7FFFFFFF, "auxiliary": 0x7FFFFFFD,
                      "audit": 0x6FFFFEFC, "depaudit": 0x6FFFFEFB}.items():
        mutated = bytearray(data)
        struct.pack_into("<qQ", mutated, tags[12], tag, 0)
        path = directory / f"{pathlib.Path(source).name}-{name}"
        path.write_bytes(mutated)
        result = subprocess.run([parser, str(path), "reject", "dependency"], check=True,
                                capture_output=True, text=True)
        if "ELF dependency has unsupported loader behavior" not in result.stdout:
            raise ValueError(f"{name} did not reach dependency tag admission: {result.stdout}")
    print(f"PASS: ordinary constructor/TLS dependency and all four presence-only tag rejections: {source}")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="spark-dependency-policy-") as temporary:
        for source_path in sys.argv[2:]:
            verify(sys.argv[1], source_path, pathlib.Path(temporary))
