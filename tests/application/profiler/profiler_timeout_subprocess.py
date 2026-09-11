import subprocess
import sys


def main():
    result = subprocess.run([sys.argv[1], "--self-destroy"], timeout=5, check=False)
    if result.returncode != 91:
        raise AssertionError(f"timer self-destruction returned {result.returncode}, expected 91")
    print("timer self-destruction failed closed with exit 91")


if __name__ == "__main__":
    main()
