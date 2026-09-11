import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github/workflows/release.yml"


def step(name):
    text = WORKFLOW.read_text(encoding="utf-8")
    block = text.split(f"      - name: {name}\n", 1)[1].split("\n      - name:", 1)[0]
    script = block.split("        run: |\n", 1)[1]
    return block, "\n".join(line[10:] for line in script.splitlines()) + "\n"


class ReleaseInputsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        git_bash = Path("C:/Program Files/Git/bin/bash.exe")
        cls.bash = str(git_bash) if os.name == "nt" and git_bash.exists() else shutil.which("bash")
        if not cls.bash:
            raise RuntimeError("Bash is required to execute the workflow regression tests")

    def run_step(self, name="Validate release", **values):
        block, script = step(name)
        self.assertNotIn("${{", script)
        if name == "Validate release":
            self.assertIn("INPUT_VERSION: ${{ inputs.version }}", block)
            self.assertIn("INPUT_DRY_RUN: ${{ inputs.dry_run }}", block)
        env = {key: os.environ[key] for key in ("PATH", "SYSTEMROOT", "WINDIR") if key in os.environ}
        env.update(
            GITHUB_EVENT_NAME="workflow_dispatch", GITHUB_REF="refs/heads/main", GITHUB_REF_NAME="main",
            INPUT_VERSION="1.2.3", INPUT_DRY_RUN="true", GITHUB_ENV="env.txt", GITHUB_OUTPUT="output.txt",
            VERSION="1.2.3", DATE="2026-01-01", TEST_EXISTING_TAG="false", TEST_DIVERGED="false",
        )
        env.update(values)
        stub = '''
git() {
  printf '%s\\n' "$*" >> git.calls
  case "$*" in
    'rev-parse --verify --quiet '*) [[ "$TEST_EXISTING_TAG" == true ]];;
    'fetch origin main develop --tags') return 0;;
    'rev-parse HEAD'|'rev-parse origin/main') echo same;;
    'rev-parse origin/develop') echo "$TEST_DIVERGED";;
    'tag --sort=-v:refname') printf 'v1.2.3\\nv1.2.2\\n';;
    *) return 99;;
  esac
}
if [[ "$TEST_DIVERGED" == false ]]; then TEST_DIVERGED=same; fi
grep() { return 0; }
diff() { return 0; }
cat() { return 0; }
'''
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [self.bash, "--noprofile", "--norc", "-e", "-o", "pipefail", "-c", stub + script],
                cwd=directory, env=env, text=True, capture_output=True, timeout=15,
            )
            files = {path.name: path.read_text() for path in Path(directory).iterdir()}
        return result, files

    def test_injected_versions_are_rejected_before_side_effects(self):
        for version in ('$(touch injected)', '`touch injected`', '"; touch injected; #',
                        '1.2.3\ntouch injected', '1.2.3"\n touch injected\n#', "1.2", "v1.2.3"):
            with self.subTest(version=version):
                result, files = self.run_step(INPUT_VERSION=version)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("Invalid version format", result.stdout)
                self.assertEqual(files, {})

    def test_dispatch_dry_run(self):
        result, files = self.run_step(GITHUB_REF="refs/heads/topic")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(files["output.txt"], "version=1.2.3\ndry_run=true\n")
        self.assertNotIn("fetch", files["git.calls"])
        self.assertIn("PREVIOUS_TAG=v1.2.2\n", files["env.txt"])

    def test_dispatch_release_and_tag_push(self):
        for event in ("workflow_dispatch", "push"):
            with self.subTest(event=event):
                result, files = self.run_step(
                    GITHUB_EVENT_NAME=event, INPUT_DRY_RUN="false", GITHUB_REF_NAME="v1.2.3",
                    GITHUB_REF="refs/tags/v1.2.3" if event == "push" else "refs/heads/main",
                    INPUT_VERSION='$(touch injected)' if event == "push" else "1.2.3",
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(files["output.txt"], "version=1.2.3\ndry_run=false\n")
                self.assertIn("fetch origin main develop --tags", files["git.calls"])
                self.assertNotIn("injected", files)
                self.assertEqual("--verify" in files["git.calls"], event == "workflow_dispatch")

    def test_release_guards(self):
        for values, message in (
            ({"INPUT_DRY_RUN": "false", "GITHUB_REF": "refs/heads/topic"}, "must run from main"),
            ({"TEST_EXISTING_TAG": "true"}, "already exists"),
            ({"INPUT_DRY_RUN": "false", "TEST_DIVERGED": "true"}, "same commit"),
            ({"GITHUB_EVENT_NAME": "push", "GITHUB_REF_NAME": "vbad"}, "Invalid version format"),
        ):
            with self.subTest(values=values):
                result, files = self.run_step(**values)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(message, result.stdout)
                self.assertNotIn("output.txt", files)
                self.assertNotIn("env.txt", files)

    def test_preview_ref_is_data(self):
        ref = 'refs/heads/$(touch injected)`touch injected`"; touch injected; #\nsecond-line'
        result, files = self.run_step("Preview", GITHUB_REF=ref)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"Ref: {ref}\n", result.stdout)
        self.assertEqual(files, {})


if __name__ == "__main__":
    unittest.main()
