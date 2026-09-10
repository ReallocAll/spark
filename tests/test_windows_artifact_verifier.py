import hashlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True


ROOT = Path(__file__).resolve().parents[1]
VERIFIER = ROOT / "tools" / "verify_windows_artifacts.ps1"
REQUIRED_TOOLS = ("pwsh", "clang-cl", "llvm-rc", "lld-link", "llvm-readobj", "llvm-pdbutil")


@unittest.skipUnless(all(shutil.which(tool) for tool in REQUIRED_TOOLS), "PowerShell and LLVM tools are required")
class WindowsArtifactVerifierTest(unittest.TestCase):
    def run_verifier(
        self, directory: Path, *extra: str, verify_hashes_only: bool = True
    ) -> subprocess.CompletedProcess[str]:
        arguments = [
            "pwsh",
            "-NoProfile",
            "-File",
            str(VERIFIER),
            "-Dll",
            str(directory / "endstone_spark.dll"),
            "-Pdb",
            str(directory / "endstone_spark.pdb"),
            "-ManifestPath",
            str(directory / "SHA256SUMS"),
        ]
        if verify_hashes_only:
            arguments.append("-VerifyHashesOnly")
        arguments.extend(extra)
        return subprocess.run(
            arguments,
            capture_output=True,
            text=True,
            check=False,
        )

    @staticmethod
    def build_fixture(directory: Path, numeric_version: tuple[int, int, int, int], string_version: str) -> None:
        major, minor, patch, private = numeric_version
        resource = f'''1 VERSIONINFO
FILEVERSION {major},{minor},{patch},{private}
PRODUCTVERSION {major},{minor},{patch},{private}
FILEOS 0x40004
FILETYPE 0x1
{{
 BLOCK "StringFileInfo"
 {{
  BLOCK "040904B0"
  {{
   VALUE "FileVersion", "{string_version}"
   VALUE "ProductVersion", "{string_version}"
  }}
 }}
 BLOCK "VarFileInfo"
 {{
  VALUE "Translation", 0x409, 1200
 }}
}}
'''
        (directory / "version.rc").write_text(resource, encoding="ascii")
        (directory / "fixture.c").write_text("int spark_fixture(void) { return 7; }\n", encoding="ascii")
        subprocess.run(
            ["llvm-rc", f"/fo{directory / 'version.res'}", str(directory / "version.rc")],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run(
            [
                "clang-cl",
                "-c",
                "-Xclang",
                "-gcodeview",
                f"-Fo{directory / 'fixture.obj'}",
                str(directory / "fixture.c"),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run(
            [
                "lld-link",
                "/dll",
                "/noentry",
                "/debug",
                f"/pdb:{directory / 'endstone_spark.pdb'}",
                f"/out:{directory / 'endstone_spark.dll'}",
                str(directory / "fixture.obj"),
                str(directory / "version.res"),
            ],
            check=True,
            capture_output=True,
            text=True,
        )

    @staticmethod
    def write_fake_pdbutil(directory: Path, guid: str, age: int) -> Path:
        script = directory / "fake-pdbutil.ps1"
        script.write_text(f'Write-Output "GUID: {{{guid}}}"\nWrite-Output "Age: {age}"\n', encoding="ascii")
        return script

    @staticmethod
    def write_manifest(directory: Path, dll: bytes, pdb: bytes) -> None:
        lines = [
            f"{hashlib.sha256(dll).hexdigest()}  endstone_spark.dll",
            f"{hashlib.sha256(pdb).hexdigest()}  endstone_spark.pdb",
        ]
        (directory / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")

    @classmethod
    def write_fixture_manifest(cls, directory: Path) -> None:
        cls.write_manifest(
            directory,
            (directory / "endstone_spark.dll").read_bytes(),
            (directory / "endstone_spark.pdb").read_bytes(),
        )

    def test_good_manifest_and_changed_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            dll = b"synthetic-dll"
            pdb = b"synthetic-pdb"
            (directory / "endstone_spark.dll").write_bytes(dll)
            (directory / "endstone_spark.pdb").write_bytes(pdb)
            self.write_manifest(directory, dll, pdb)

            good = self.run_verifier(directory)
            self.assertEqual(good.returncode, 0, good.stdout + good.stderr)

            (directory / "endstone_spark.dll").write_bytes(b"same-guid-different-bytes")
            changed = self.run_verifier(directory)
            self.assertNotEqual(changed.returncode, 0)
            self.assertIn("hash mismatch", changed.stdout + changed.stderr)

    def test_manifest_rejects_duplicate_and_unsafe_entries(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            dll = b"synthetic-dll"
            pdb = b"synthetic-pdb"
            (directory / "endstone_spark.dll").write_bytes(dll)
            (directory / "endstone_spark.pdb").write_bytes(pdb)
            dll_hash = hashlib.sha256(dll).hexdigest()
            pdb_hash = hashlib.sha256(pdb).hexdigest()

            (directory / "SHA256SUMS").write_text(
                f"{dll_hash}  endstone_spark.dll\n{pdb_hash}  endstone_spark.dll\n", encoding="utf-8"
            )
            duplicate = self.run_verifier(directory)
            self.assertNotEqual(duplicate.returncode, 0)
            self.assertIn("duplicate", duplicate.stdout + duplicate.stderr)

            (directory / "SHA256SUMS").write_text(
                f"{dll_hash}  endstone_spark.dll\n{pdb_hash}  ..\\endstone_spark.pdb\n", encoding="utf-8"
            )
            unsafe = self.run_verifier(directory)
            self.assertNotEqual(unsafe.returncode, 0)
            self.assertIn("unsafe", unsafe.stdout + unsafe.stderr)

    def test_full_verification_accepts_matching_guid_age_and_version(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.build_fixture(directory, (1, 2, 3, 0), "1.2.3.0")
            self.write_fixture_manifest(directory)

            result = self.run_verifier(directory, "-Version", "1.2.3", verify_hashes_only=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_full_verification_rejects_mismatched_guid_and_age(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.build_fixture(directory, (1, 2, 3, 0), "1.2.3.0")
            self.write_fixture_manifest(directory)
            fake_pdbutil = self.write_fake_pdbutil(directory, "00000000-0000-0000-0000-000000000000", 2)

            result = self.run_verifier(
                directory,
                "-Version",
                "1.2.3",
                "-LlvmPdbUtil",
                str(fake_pdbutil),
                verify_hashes_only=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("GUID+Age", result.stdout + result.stderr)

    def test_full_verification_rejects_string_version_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.build_fixture(directory, (1, 2, 3, 0), "1.2.4.0")
            self.write_fixture_manifest(directory)

            result = self.run_verifier(directory, "-Version", "1.2.3", verify_hashes_only=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("FileVersion/ProductVersion", result.stdout + result.stderr)

    def test_full_verification_rejects_numeric_version_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.build_fixture(directory, (1, 2, 4, 0), "1.2.3.0")
            self.write_fixture_manifest(directory)

            result = self.run_verifier(directory, "-Version", "1.2.3", verify_hashes_only=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("numeric fields", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
