import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


EXECUTABLE = None
NT_SIZE = 264


def dos_file(size, offset):
    data = bytearray(size)
    struct.pack_into("<H", data, 0, 0x5A4D)
    struct.pack_into("<i", data, 60, offset)
    return data


def pe_file(with_section):
    offset = 64
    data = dos_file(0x6000 if with_section else offset + NT_SIZE, offset)
    struct.pack_into("<I", data, offset, 0x4550)
    struct.pack_into("<HH", data, offset + 4, 0x8664, 4 if with_section else 0)
    struct.pack_into("<H", data, offset + 20, 240)
    optional = offset + 24
    struct.pack_into("<H", data, optional, 0x20B)
    struct.pack_into("<Q", data, optional + 24, 0x180000000)
    struct.pack_into("<II", data, optional + 32, 0x1000, 0x200)
    struct.pack_into("<II", data, optional + 56, 0x8000, 0x400 if with_section else len(data))
    struct.pack_into("<I", data, optional + 108, 16)
    if with_section:
        for index, (name, rva, size, flags) in enumerate((
            (b".text", 0x1000, 0x1000, 0x60000020),
            (b".rdata", 0x2000, 0x2000, 0x40000040),
            (b".pdata", 0x4000, 0x1000, 0x40000040),
            (b".xdata", 0x5000, 0x1000, 0x40000040),
        )):
            section = offset + NT_SIZE + index * 40
            data[section:section + 8] = name.ljust(8, b"\0")
            struct.pack_into("<IIII", data, section + 8, size, rva, size, rva)
            struct.pack_into("<I", data, section + 36, flags)
        data[0x1000] = 0xC3
        struct.pack_into("<II", data, optional + 112 + 3 * 8, 0x4000, 12)
        struct.pack_into("<III", data, 0x4000, 0x1000, 0x1010, 0x5000)
        data[0x5000] = 1
    return data


class PeEvaluatorTest(unittest.TestCase):
    def evaluate(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "synthetic.exe"
            path.write_bytes(data)
            return subprocess.run(
                [str(EXECUTABLE), "--evaluate", str(path), "0x1000"],
                capture_output=True, text=True, timeout=15,
            )

    def test_truncated_headers(self):
        cases = [(b"", "invalid PE size"), (b"MZ", "unable to read PE")]
        cases.extend((dos_file(size, offset), "invalid DOS/NT headers") for size, offset in (
            (64, 0), (64, -1), (64, 64), (64, 65), (64, 0x7FFFFFFF), (64 + NT_SIZE - 1, 64),
        ))
        for data, message in cases:
            with self.subTest(size=len(data), message=message):
                result = self.evaluate(data)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(message, result.stderr)
                self.assertEqual(result.stdout, "")

    def test_exact_boundary_preserves_engine_rejection(self):
        result = self.evaluate(pe_file(False))
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn("symbol guess engine rejected PE", result.stderr)

    def test_valid_synthetic_pe(self):
        result = self.evaluate(pe_file(True))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "0x1000\t\n")
        self.assertIn("ranges=1", result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True, type=Path)
    args, remaining = parser.parse_known_args()
    EXECUTABLE = args.executable.resolve(strict=True)
    unittest.main(argv=[__file__, *remaining])
