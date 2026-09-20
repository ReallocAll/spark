#!/usr/bin/env python3
"""Verify that source layers respect architectural boundaries.

Dependency model: platform/endstone -> application -> core/proto/net -> native

- src/native/      may only include from src/native/
- src/core/,       src/proto/, and src/net/ may include from the shared core
                   layer and src/native/
- src/application/ may include from src/application/, the shared core layer,
                   and src/native/
- src/platform/    may include from anywhere
- src/platform/endstone/ may include from anywhere

No layer below platform/ may include <endstone/...> or "platform/endstone/...".
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)

# Layers and what they may include (prefixes allowed).
LAYER_RULES = {
    "native": {"native/"},
    "core": {"core/", "proto/", "net/", "native/"},
    "application": {"application/", "core/", "proto/", "net/", "native/"},
}

FORBIDDEN_PATTERNS = [
    re.compile(r"^endstone(?:/|$)"),
    re.compile(r"^endstone_papi(?:/|$)"),
    re.compile(r"^papi(?:/|$)"),
    re.compile(r"^platform/endstone/"),
    re.compile(r"^platform/"),
]

INTERNAL_PREFIXES = ("core/", "proto/", "net/", "native/", "application/", "platform/")
EXACT_LAYER_INCLUDES = {"native": {"core/profiler/profiling_window.h"}}
SHARED_INTERFACE_PATHS = {"core/profiler/profiling_window.h"}


def _is_project_internal_include(include: str) -> bool:
    return any(include.startswith(prefix) for prefix in INTERNAL_PREFIXES)


def layer_of(path: Path) -> str | None:
    rel = path.relative_to(SRC)
    parts = rel.parts
    if parts[0] == "native":
        return "native"
    if parts[0] in {"core", "proto", "net"}:
        return "core"
    if parts[0] == "application":
        return "application"
    return None


def check_text(relative: str, text: str) -> list[str]:
    path = ROOT / relative
    layer = layer_of(path)
    if layer is None:
        return []
    allowed = LAYER_RULES[layer]
    violations = []
    source_relative = path.relative_to(SRC).as_posix()
    for m in INCLUDE_RE.finditer(text):
        inc = m.group(1)
        if source_relative in SHARED_INTERFACE_PATHS and _is_project_internal_include(inc):
            violations.append(f"{relative}: shared interface includes <{inc}> (project-internal include forbidden)")
            continue
        # Check forbidden patterns first.
        for pat in FORBIDDEN_PATTERNS:
            if pat.match(inc):
                violations.append(f"{relative}: includes <{inc}> (forbidden in {layer} layer)")
                break
        else:
            if inc in EXACT_LAYER_INCLUDES.get(layer, set()):
                continue
            # System/library includes (no slash or known external libs) are always fine.
            if "/" not in inc and not inc.startswith("endstone"):
                continue
            # Check if the include is allowed by the layer's rules.
            # spark-internal includes use paths like "core/...", "native/...", etc.
            if not any(inc.startswith(prefix) for prefix in allowed):
                # External library includes (cpptrace, etc.) don't match any
                # internal prefix and are fine.
                if not _is_project_internal_include(inc):
                    continue
                violations.append(
                    f"{relative}: includes <{inc}> (not allowed in {layer} layer)"
                )
    return violations


def check_file(path: Path) -> list[str]:
    return check_text(path.relative_to(ROOT).as_posix(), path.read_text(encoding="utf-8", errors="replace"))


def main() -> int:
    extensions = {".h", ".hpp", ".cpp", ".cc"}
    files = [p for p in SRC.rglob("*") if p.suffix in extensions and layer_of(p) is not None]
    violations = []
    for f in sorted(files):
        violations.extend(check_file(f))
    if violations:
        for v in violations:
            print(v, file=sys.stderr)
        print(f"\n{len(violations)} architectural boundary violation(s) found.", file=sys.stderr)
        return 1
    print(f"OK: {len(files)} files checked, no boundary violations.")
    return 0


def test_native_allows_only_the_exact_shared_profiling_header() -> None:
    assert check_text("src/native/fixture.cpp", '#include "core/profiler/profiling_window.h"\n') == []


def test_native_rejects_shared_header_prefixes() -> None:
    for include in ("core/profiler/profiler.h", "core/profiler/profiling_window.h.extra"):
        violations = check_text("src/native/fixture.cpp", f'#include "{include}"\n')
        assert violations


def test_shared_interface_rejects_project_internal_includes() -> None:
    violations = check_text(
        "src/core/profiler/profiling_window.h",
        '#include "core/util/format.h"\n',
    )
    assert violations


if __name__ == "__main__":
    sys.exit(main())
