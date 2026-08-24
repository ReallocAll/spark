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

    def test_release_is_published_after_both_artifacts(self):
        workflow = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
        self.assertNotIn("conanfile.txt", workflow)
        self.assertNotIn("gh release upload", workflow)
        self.assertEqual(workflow.count("gh release create"), 1)
        self.assertIn("needs: [release, windows, linux]", workflow)
        self.assertLess(workflow.index("  windows:"), workflow.index("  publish:"))
        self.assertLess(workflow.index("  linux:"), workflow.index("  publish:"))
        self.assertIn("ctest --test-dir build/RelWithDebInfo --output-on-failure", workflow)

if __name__ == "__main__":
    unittest.main()
