#!/usr/bin/env python3
"""Measure Rust unsafe source syntax, not memory safety or expanded code.

By default only kernel/ is scanned. Comments/literals retain their original
positions while being masked; all cfg branches and macro definitions are
counted as written. Known u!(...), u![...], and u!{...} invocations count as
unsafe scopes. Other macro expansion and implicit unsafe operations are not
inferred. An unsafe impl/trait/extern declaration does not make its body an
unsafe executable scope. Percentages count physical lines spanned by unsafe
bodies (including comments and delimiters), not the number of unsafe operations.

Examples:
    python3 unsafe_analyzer.py --json > /tmp/unsafe-before.json
    python3 unsafe_analyzer.py --baseline /tmp/unsafe-before.json
    python3 unsafe_analyzer.py kernel/net.rs kernel/sync --json
"""

from __future__ import annotations

import argparse
import bisect
from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
import re
from typing import Iterator

SCHEMA_VERSION = 1
CHAR_LITERAL = re.compile(r"'(?:\\(?:u\{[0-9a-fA-F_]+\}|x[0-9a-fA-F]{2}|[^\n])|[^'\\\n])'")
RAW_STRING = re.compile(r'(?:br|cr|r)(#*)"')
OPENERS = {"(": ")", "[": "]", "{": "}"}


def identifier_start(char: str) -> bool:
    return char == "_" or char.isidentifier()


def identifier_continue(char: str) -> bool:
    return ("_" + char).isidentifier()


def mask_non_code(source: str) -> str:
    """Replace comments and literal contents with spaces, retaining newlines."""
    masked = list(source)

    def erase(start: int, end: int) -> None:
        for index in range(start, end):
            if source[index] not in "\r\n":
                masked[index] = " "

    index = 0
    while index < len(source):
        start = index
        if source.startswith("//", index):
            end = source.find("\n", index + 2)
            index = len(source) if end == -1 else end
        elif source.startswith("/*", index):
            index += 2
            depth = 1
            while index < len(source) and depth:
                if source.startswith("/*", index):
                    depth += 1
                    index += 2
                elif source.startswith("*/", index):
                    depth -= 1
                    index += 2
                else:
                    index += 1
            if depth:
                raise ValueError(f"unterminated block comment at offset {start}")
        else:
            raw = RAW_STRING.match(source, index)
            if raw and (index == 0 or not identifier_continue(source[index - 1])):
                closing = '"' + raw.group(1)
                end = source.find(closing, raw.end())
                if end == -1:
                    raise ValueError(f"unterminated raw string at offset {start}")
                index = end + len(closing)
            elif source[index] == '"':
                index += 1
                while index < len(source):
                    if source[index] == "\\":
                        index += 2
                    elif source[index] == '"':
                        index += 1
                        break
                    else:
                        index += 1
                else:
                    raise ValueError(f"unterminated string at offset {start}")
            elif source[index] == "'" and (char := CHAR_LITERAL.match(source, index)):
                index = char.end()
            else:
                index += 1
                continue
        erase(start, min(index, len(source)))
    return "".join(masked)


@dataclass(frozen=True)
class Token:
    text: str
    start: int
    end: int


def tokens(code: str) -> list[Token]:
    result = []
    index = 0
    while index < len(code):
        if code[index].isspace():
            index += 1
            continue
        start = index
        # Raw identifiers such as r#unsafe are names, never unsafe keywords.
        if code.startswith("r#", index) and index + 2 < len(code) and identifier_start(code[index + 2]):
            index += 2
        if identifier_start(code[index]):
            index += 1
            while index < len(code) and identifier_continue(code[index]):
                index += 1
        elif code.startswith(("->", "=>", "::"), index):
            index += 2
        else:
            index += 1
        result.append(Token(code[start:index], start, index))
    return result


def delimiter_pairs(items: list[Token]) -> dict[int, int]:
    """Match source delimiters once; strings and comments are already absent."""
    stack: list[int] = []
    pairs = {}
    for index, token in enumerate(items):
        if token.text in OPENERS:
            stack.append(index)
        elif token.text in OPENERS.values():
            if not stack or OPENERS[items[stack[-1]].text] != token.text:
                raise ValueError(f"unmatched {token.text!r} at offset {token.start}")
            pairs[stack.pop()] = index
    if stack:
        token = items[stack[-1]]
        raise ValueError(f"unclosed {token.text!r} at offset {token.start}")
    return pairs


def function_body(items: list[Token], start: int, pairs: dict[int, int]) -> int | None:
    """Find a function body, skipping parameter and const-generic delimiters."""
    # A function-pointer type has no item name: its `unsafe fn(...)` can
    # appear inside a safe function's parameters or initializer. Do not
    # mistake the enclosing function's later brace for this type's body.
    if start >= len(items) or items[start].text == "(":
        return None
    index = start
    angle_depth = 0
    while index < len(items):
        word = items[index].text
        if word == ";" or word == "}":
            return None
        if word == "<":
            angle_depth += 1
        elif word == ">" and angle_depth:
            angle_depth -= 1
        elif word == "{" and not angle_depth:
            return index
        elif word in OPENERS:
            index = pairs[index]
        index += 1
    return None


@dataclass(frozen=True)
class Metrics:
    total_lines: int = 0
    unsafe_lines: int = 0
    unsafe_keywords: int = 0
    unsafe_blocks: int = 0
    unsafe_functions: int = 0
    unsafe_function_declarations: int = 0
    unsafe_impls: int = 0
    unsafe_traits: int = 0
    unsafe_externs: int = 0
    unsafe_macros: int = 0

    @property
    def percentage(self) -> float:
        return 100.0 * self.unsafe_lines / self.total_lines if self.total_lines else 0.0

    @property
    def unsafe_sites_per_kloc(self) -> float:
        sites = self.unsafe_keywords + self.unsafe_macros
        return 1000.0 * sites / self.total_lines if self.total_lines else 0.0

    def report(self) -> dict[str, int | float]:
        return dict(asdict(self), percentage=round(self.percentage, 3),
                    unsafe_sites_per_kloc=round(self.unsafe_sites_per_kloc, 3))


def unsafe_line_numbers(source: str) -> tuple[Metrics, set[int]]:
    """Return metrics and the one-based physical lines covered by unsafe bodies."""
    items = tokens(mask_non_code(source))
    pairs = delimiter_pairs(items)
    counts = dict.fromkeys(asdict(Metrics()), 0)
    counts["total_lines"] = source.count("\n") + int(bool(source) and not source.endswith("\n"))
    line_offsets = [0] + [match.end() for match in re.finditer("\n", source)]
    covered: set[int] = set()

    def cover(opener: int) -> None:
        first = bisect.bisect_right(line_offsets, items[opener].start)
        last = bisect.bisect_right(line_offsets, items[pairs[opener]].start)
        covered.update(range(first, last + 1))

    for index, token in enumerate(items):
        following = index + 1
        if token.text == "u" and following + 1 < len(items) and items[following].text == "!":
            opener = following + 1
            if items[opener].text in OPENERS:
                counts["unsafe_macros"] += 1
                cover(opener)
        if token.text != "unsafe":
            continue
        counts["unsafe_keywords"] += 1
        if following == len(items):
            continue
        word = items[following].text
        if word == "{":
            counts["unsafe_blocks"] += 1
            cover(following)
        elif word in ("impl", "trait"):
            counts[f"unsafe_{word}s"] += 1
        else:
            if word == "extern":
                following += 1  # The optional ABI string was masked.
                if following >= len(items) or items[following].text != "fn":
                    counts["unsafe_externs"] += 1
                    continue
                word = "fn"
            if word == "fn":
                body = function_body(items, following + 1, pairs)
                if body is None:
                    counts["unsafe_function_declarations"] += 1
                else:
                    counts["unsafe_functions"] += 1
                    cover(body)
    counts["unsafe_lines"] = len(covered)
    return Metrics(**counts), covered


def analyze_source(source: str) -> Metrics:
    return unsafe_line_numbers(source)[0]


def analyze_unsafe(filepath: str | Path) -> tuple[float, int, int]:
    """Compatibility interface: (scope-line percentage, scope lines, all lines)."""
    metric = analyze_source(Path(filepath).read_text(encoding="utf-8"))
    return metric.percentage, metric.unsafe_lines, metric.total_lines


def excluded(name: str) -> bool:
    return name in {"target", "build", ".git"} or name.startswith(("build_", "build-", "cmake-build-"))


def rust_files(paths: list[Path]) -> Iterator[Path]:
    found: set[Path] = set()
    for path in paths:
        if not path.exists():
            raise ValueError(f"path does not exist: {path}")
        if any(excluded(part) for part in path.parts):
            continue
        if path.is_file():
            if path.suffix == ".rs":
                found.add(path.resolve())
            continue
        for directory, directories, names in os.walk(path):
            directories[:] = sorted(name for name in directories if not excluded(name))
            for name in names:
                if name.endswith(".rs"):
                    found.add((Path(directory) / name).resolve())
    yield from sorted(found)


def make_report(paths: list[Path]) -> dict:
    results = []
    totals = dict.fromkeys(asdict(Metrics()), 0)
    for path in rust_files(paths):
        try:
            metric = analyze_source(path.read_text(encoding="utf-8"))
        except ValueError as error:
            raise ValueError(f"{path}: {error}") from error
        results.append(dict(path=os.path.relpath(path), **metric.report()))
        for key, value in asdict(metric).items():
            totals[key] += value
    return {
        "schema_version": SCHEMA_VERSION,
        "scope": "lexical Rust source, all cfg branches; u! recognized; no macro expansion or safety proof",
        "totals": dict(files=len(results), **Metrics(**totals).report()),
        "files": results,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="*", type=Path, help="Rust files/directories (default: repository kernel/)")
    parser.add_argument("--json", action="store_true", help="emit deterministic machine-readable metrics")
    parser.add_argument("--baseline", type=Path, help="compare totals against a prior --json report")
    args = parser.parse_args()
    try:
        report = make_report(args.paths or [Path(__file__).resolve().parent / "kernel"])
        if args.baseline:
            baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
            if baseline.get("schema_version") != SCHEMA_VERSION:
                raise ValueError("baseline schema differs; regenerate it using this analyzer")
            report["delta"] = {key: round(value - baseline["totals"][key], 3)
                               for key, value in report["totals"].items()}
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
        return
    for row in sorted(report["files"], key=lambda row: (-row["percentage"], row["path"])):
        if row["unsafe_lines"]:
            print(f'{row["path"]}: {row["percentage"]:.1f}% '
                  f'({row["unsafe_lines"]}/{row["total_lines"]} lines span unsafe bodies)')
    total = report["totals"]
    print(f'Total: {total["unsafe_lines"]}/{total["total_lines"]} lines '
          f'({total["percentage"]:.3f}%); {total["unsafe_keywords"]} unsafe keywords, '
          f'{total["unsafe_macros"]} u! calls; {total["unsafe_sites_per_kloc"]:.3f} sites/1k lines')
    if "delta" in report:
        print("Baseline delta: " + json.dumps(report["delta"], sort_keys=True))
    print("Scope: " + report["scope"])


if __name__ == "__main__":
    main()
