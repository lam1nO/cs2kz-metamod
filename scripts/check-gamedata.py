#!/usr/bin/env python3
"""Проверка Linux-сигнатур по реальным бинарникам перед переходом на новую CS2."""

import argparse
import json
from pathlib import Path
import re
import sys


def pattern(value):
    tokens = re.findall(r"\\x([0-9a-fA-F]{2})", value)
    escaped = bool(tokens)
    if not escaped:
        tokens = value.split()
    if not tokens:
        raise ValueError("empty signature")
    return b"".join(
        b"." if token in ("?", "??") or (escaped and token.lower() == "2a")
        else re.escape(bytes.fromhex(token)) for token in tokens
    )


def signatures(path):
    text = path.read_text()
    if path.suffix == ".json":
        for name, entry in json.loads(text).items():
            sig = entry.get("signatures", {})
            if sig.get("linux"):
                yield name, sig["library"], sig["linux"], False
    else:
        # Сигнатуры — листовые блоки KV. Offsets/Patches без library не читаем.
        text = re.sub(r"//[^\n]*", "", text)
        for name, body in re.findall(r'"([^"\n]+)"\s*\{([^{}]*)\}', text):
            fields = dict(re.findall(r'"([^"\n]+)"\s*"([^"\n]*)"', body))
            if fields.get("library") and fields.get("linux"):
                yield name, fields["library"], fields["linux"], fields.get("allow_multi_match") == "1"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gamedata", type=Path, action="append")
    parser.add_argument("--library", action="append", required=True, metavar="NAME=FILE")
    args = parser.parse_args()
    libraries = {}
    for value in args.library:
        name, file = value.split("=", 1)
        libraries[name] = Path(file).read_bytes()
    paths = args.gamedata or [Path(__file__).resolve().parents[1] / "gamedata/cs2kz-core.games.txt"]
    checked = failed = skipped = 0
    for path in paths:
        entries = list(signatures(path))
        if not entries:
            parser.error(f"no signatures in {path}")
        for name, library, value, allow_multiple in entries:
            if library not in libraries:
                print(f"SKIP {path.name}:{name}: library {library} not supplied")
                skipped += 1
                continue
            if value.startswith("@"):
                print(f"SKIP {path.name}:{name}: symbol requires runtime resolution")
                skipped += 1
                continue
            matches = sum(1 for _ in re.finditer(pattern(value), libraries[library], re.DOTALL))
            checked += 1
            if matches == 0 or (matches > 1 and not allow_multiple):
                print(f"FAIL {path.name}:{name}: matches={matches}")
                failed += 1
    print(f"checked={checked} failed={failed} skipped={skipped}")
    return 1 if failed or not checked else 0


if __name__ == "__main__":
    sys.exit(main())
