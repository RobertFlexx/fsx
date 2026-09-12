#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent
DIRS = (ROOT / "src", ROOT / "include", ROOT / "tests")
EXTS = {".c", ".cc", ".cpp", ".h", ".hh", ".hpp"}
MAX_LINE = 120
errors = []
checked = 0

for base in DIRS:
    for path in sorted(base.rglob("*")):
        if not path.is_file() or path.suffix not in EXTS:
            continue
        checked += 1
        raw = path.read_bytes()
        rel = path.relative_to(ROOT)
        if not raw.endswith(b"\n"):
            errors.append(f"{rel}: missing final newline")
        try:
            text = raw.decode("ascii")
        except UnicodeDecodeError as exc:
            errors.append(f"{rel}: non-ASCII source byte at offset {exc.start}")
            continue
        if "\r" in text:
            errors.append(f"{rel}: CR character found")
        for lineno, line in enumerate(text.splitlines(), 1):
            if "\t" in line:
                errors.append(f"{rel}:{lineno}: tab character found")
            if line.rstrip() != line:
                errors.append(f"{rel}:{lineno}: trailing whitespace")
            if len(line) > MAX_LINE:
                errors.append(f"{rel}:{lineno}: line is {len(line)} columns (limit {MAX_LINE})")

if errors:
    for item in errors:
        print(item, file=sys.stderr)
    print(f"style check: FAIL ({len(errors)} issue(s), {checked} files)", file=sys.stderr)
    raise SystemExit(1)
print(f"style check: PASS ({checked} files, ASCII, no tabs/trailing whitespace, <= {MAX_LINE} columns)")
