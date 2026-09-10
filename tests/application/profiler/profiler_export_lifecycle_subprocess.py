import subprocess
import sys


EXPECTED_EXIT_CODE = 91


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: profiler_export_lifecycle_subprocess.py <test-executable>", file=sys.stderr)
        return 2
    try:
        completed = subprocess.run(
            [sys.argv[1], "--destructor-fail-closed"],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
    except subprocess.TimeoutExpired:
        print("destructor fail-closed child exceeded watchdog", file=sys.stderr)
        return 1
    if completed.returncode != EXPECTED_EXIT_CODE:
        print(
            f"expected destructor fail-closed exit {EXPECTED_EXIT_CODE}, got {completed.returncode}",
            file=sys.stderr,
        )
        if completed.stdout:
            print(completed.stdout, file=sys.stderr, end="")
        if completed.stderr:
            print(completed.stderr, file=sys.stderr, end="")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
