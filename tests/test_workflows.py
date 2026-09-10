import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class WorkflowTest(unittest.TestCase):
    def test_build_enforces_project_quality(self):
        workflow = (ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")
        self.assertNotIn("conanfile.txt", workflow)
        self.assertEqual(workflow.count("conanfile.py"), 4)
        self.assertEqual(workflow.count("tools/run_clang_tidy.py"), 2)
        self.assertEqual(workflow.count("--shard ${{ matrix.shard }}"), 2)
        self.assertEqual(workflow.count("shard: [core, native, application, tests-core, selftest]"), 2)
        self.assertEqual(workflow.count("--dry-run --Werror"), 2)
        self.assertEqual(workflow.count("CMAKE_EXPORT_COMPILE_COMMANDS"), 4)
        self.assertEqual(workflow.count("CMAKE_CXX_SCAN_FOR_MODULES:BOOL=OFF"), 2)
        self.assertIn("name: Build & test Linux", workflow)
        self.assertIn("name: Build & test Windows", workflow)
        self.assertNotIn("needs: linux", workflow)
        self.assertNotIn("needs: windows", workflow)
        self.assertEqual(workflow.count("tools/verify_windows_artifacts.ps1"), 1)
        self.assertIn("build/RelWithDebInfo/SHA256SUMS", workflow)

    def test_release_is_published_after_both_artifacts(self):
        workflow = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
        self.assertNotIn("conanfile.txt", workflow)
        self.assertNotIn("gh release upload", workflow)
        self.assertEqual(workflow.count("gh release create"), 1)
        self.assertIn("needs: [release, windows, linux]", workflow)
        self.assertLess(workflow.index("  windows:"), workflow.index("  publish:"))
        self.assertLess(workflow.index("  linux:"), workflow.index("  publish:"))
        self.assertIn("ctest --test-dir build/RelWithDebInfo --output-on-failure", workflow)
        self.assertEqual(workflow.count("tools/verify_windows_artifacts.ps1"), 2)
        self.assertIn("cp release-windows/SHA256SUMS release-windows/SHA256SUMS-windows", workflow)
        self.assertIn("cmp -- release-windows/SHA256SUMS release-windows/SHA256SUMS-windows", workflow)
        self.assertIn("release-windows/SHA256SUMS-windows", workflow)
        self.assertNotIn("SHA256SUMS#SHA256SUMS-windows", workflow)

    def test_windows_version_resource_is_configure_time_generated(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        resource = (ROOT / "src" / "version.rc.in").read_text(encoding="utf-8")
        self.assertIn("configure_file(src/version.rc.in", cmake)
        self.assertIn('target_sources(spark PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")', cmake)
        self.assertIn("@PROJECT_VERSION_MAJOR@", resource)
        self.assertIn("@PROJECT_VERSION_MINOR@", resource)
        self.assertIn("@PROJECT_VERSION_PATCH@", resource)
        self.assertIn('VALUE "FileVersion", "@PROJECT_VERSION@.0\\0"', resource)
        self.assertIn('VALUE "ProductVersion", "@PROJECT_VERSION@.0\\0"', resource)
        self.assertNotIn("0.5.3", resource)

if __name__ == "__main__":
    unittest.main()
