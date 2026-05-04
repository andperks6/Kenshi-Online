"""
Audit high-signal records in kenshi-online.mod without modifying it.

Kenshi .mod files are binary FCS records with length-prefixed strings. This
script intentionally does not try to parse or rewrite the full format; it
extracts printable strings and reports the records that matter for the current
multiplayer cleanup decision.
"""

from __future__ import annotations

import argparse
import os
import re
from collections import Counter
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MODS = [
    REPO_ROOT / "kenshi-online.mod",
    REPO_ROOT / "dist" / "kenshi-online.mod",
]

INTERESTING_NAMES = [
    "Singleplayer",
    "Multiplayer",
    "Player 1",
    "Player 2",
    "Player 1 squad",
    "Player 2 squad",
    "startoff- Wanderer dead",
    "startoff- Wanderer squad copy",
    "Bonedog dead",
    "TEST7ER",
    "REMOVED",
]


def extract_printable_strings(data: bytes, min_len: int = 4) -> list[tuple[int, str]]:
    pattern = rb"[\x20-\x7e]{" + str(min_len).encode("ascii") + rb",}"
    return [(m.start(), m.group(0).decode("ascii", errors="replace")) for m in re.finditer(pattern, data)]


def count_mod_refs(strings: list[tuple[int, str]]) -> Counter[str]:
    refs: Counter[str] = Counter()
    for _, text in strings:
        for ref in re.findall(r"\b\d+-(?:kenshi-online|gamedata|rebirth|Newwworld|Dialogue|TwoStorey)\.mod\b", text):
            refs[ref] += 1
    return refs


def print_context(strings: list[tuple[int, str]], needle: str, width: int = 3) -> None:
    matches = [i for i, (_, text) in enumerate(strings) if needle in text]
    if not matches:
        print(f"  {needle}: not found")
        return

    print(f"  {needle}: {len(matches)} occurrence(s)")
    for match_idx in matches[:4]:
        start = max(0, match_idx - width)
        end = min(len(strings), match_idx + width + 1)
        for off, text in strings[start:end]:
            trimmed = text if len(text) <= 100 else text[:97] + "..."
            marker = "=>" if needle in text else "  "
            print(f"    {marker} 0x{off:06X} {trimmed}")
        if len(matches) > 4:
            print(f"    ... {len(matches) - 4} more")
            break


def audit(path: Path) -> int:
    if not path.exists():
        print(f"{path}: missing")
        return 1

    data = path.read_bytes()
    strings = extract_printable_strings(data)
    refs = count_mod_refs(strings)

    print(f"{path}")
    print(f"  size: {len(data)} bytes")
    print(f"  printable strings: {len(strings)}")
    print(f"  REMOVED markers: {sum(1 for _, text in strings if text == 'REMOVED')}")
    print()

    print("Interesting records:")
    for name in INTERESTING_NAMES:
        print_context(strings, name, width=1)
    print()

    print("kenshi-online references:")
    for ref, count in sorted((item for item in refs.items() if "kenshi-online" in item[0]), key=lambda item: item[0]):
        print(f"  {ref}: {count}")
    print()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mods", nargs="*", type=Path, help="mod files to audit")
    args = parser.parse_args()

    paths = args.mods or DEFAULT_MODS
    status = 0
    for i, path in enumerate(paths):
        if i:
            print("=" * 80)
        status |= audit(path)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
