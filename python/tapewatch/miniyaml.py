"""A YAML subset loader, standard library only.

The whole Tapewatch Python side has no third-party dependencies. That is a
deliberate constraint rather than an accident: the harness has to run from a
bare `python3` with no pip, no virtualenv and no network, on a machine where
installing anything is not allowed. PyYAML would be one line in a
requirements file and one more reason the pipeline cannot run.

What is supported is what the config files actually use:

    key: value            scalars: int, float, bool, null, quoted/bare string
    key:                  nested mappings, by indentation
      inner: 1
    key: [1, 2, 3]        inline lists
    key: {a: 1, b: 2}     inline mappings of scalars
    key:                  block lists of scalars or of mappings
      - 1
      - name: a
        n: 2
    # comments, blank lines

What is not supported raises ValueError with a line number rather than
quietly producing something else. Anchors, multi-document files, block
scalars, flow mappings and tags are all in that category. If a config ever
needs one of them, this file is the wrong answer and PyYAML is the right one.
"""

from __future__ import annotations

import re
from typing import Any

_INLINE_LIST = re.compile(r"^\[(.*)\]$")
_INLINE_MAP = re.compile(r"^\{(.*)\}$")


class YamlError(ValueError):
    pass


def _scalar(text: str, line_no: int) -> Any:
    t = text.strip()
    if not t:
        return None
    if t[0] in "\"'":
        if len(t) < 2 or t[-1] != t[0]:
            raise YamlError(f"line {line_no}: unterminated quoted string")
        return t[1:-1]
    low = t.lower()
    if low in ("null", "~"):
        return None
    if low == "true":
        return True
    if low == "false":
        return False
    m = _INLINE_LIST.match(t)
    if m:
        body = m.group(1).strip()
        if not body:
            return []
        return [_scalar(p, line_no) for p in body.split(",")]
    m = _INLINE_MAP.match(t)
    if m:
        body = m.group(1).strip()
        if not body:
            return {}
        out: dict[str, Any] = {}
        for pair in body.split(","):
            if ":" not in pair:
                raise YamlError(f"line {line_no}: expected 'key: value' in {pair!r}")
            k, _, v = pair.partition(":")
            out[k.strip()] = _scalar(v, line_no)
        return out
    try:
        return int(t)
    except ValueError:
        pass
    try:
        return float(t)
    except ValueError:
        pass
    return t


class _Line:
    __slots__ = ("indent", "text", "no")

    def __init__(self, indent: int, text: str, no: int) -> None:
        self.indent = indent
        self.text = text
        self.no = no


def _lines(text: str) -> list[_Line]:
    out: list[_Line] = []
    for i, raw in enumerate(text.splitlines(), start=1):
        if "\t" in raw[: len(raw) - len(raw.lstrip())]:
            raise YamlError(f"line {i}: tab in indentation")
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            continue
        # A '#' inside quotes is not a comment. Nothing in these configs needs
        # that, but getting it wrong would silently truncate a value.
        if "#" in stripped and not stripped.split("#", 1)[0].count('"') % 2:
            stripped = stripped.split("#", 1)[0].rstrip()
        out.append(_Line(len(raw) - len(raw.lstrip()), stripped, i))
    return out


def _parse_block(lines: list[_Line], pos: int, indent: int) -> tuple[Any, int]:
    if pos >= len(lines):
        return None, pos
    if lines[pos].text.startswith("- "):
        return _parse_list(lines, pos, indent)
    return _parse_map(lines, pos, indent)


def _parse_list(lines: list[_Line], pos: int, indent: int) -> tuple[list, int]:
    items: list[Any] = []
    while pos < len(lines) and lines[pos].indent == indent and lines[pos].text.startswith("- "):
        ln = lines[pos]
        body = ln.text[2:].strip()
        if ":" in body and not body.startswith(("'", '"')):
            # A mapping whose first key shares the dash's line. Re-present it
            # as an ordinary block at the column the key starts in.
            key_indent = indent + 2
            synthetic = [_Line(key_indent, body, ln.no)]
            pos += 1
            while pos < len(lines) and lines[pos].indent >= key_indent:
                synthetic.append(lines[pos])
                pos += 1
            value, _ = _parse_map(synthetic, 0, key_indent)
            items.append(value)
            continue
        items.append(_scalar(body, ln.no))
        pos += 1
    return items, pos


def _parse_map(lines: list[_Line], pos: int, indent: int) -> tuple[dict, int]:
    out: dict[str, Any] = {}
    while pos < len(lines) and lines[pos].indent == indent:
        ln = lines[pos]
        if ln.text.startswith("- "):
            break
        if ":" not in ln.text:
            raise YamlError(f"line {ln.no}: expected 'key: value', got {ln.text!r}")
        key, _, rest = ln.text.partition(":")
        key = key.strip()
        rest = rest.strip()
        pos += 1
        if rest:
            out[key] = _scalar(rest, ln.no)
            continue
        if pos < len(lines) and lines[pos].indent > indent:
            value, pos = _parse_block(lines, pos, lines[pos].indent)
            out[key] = value
        else:
            out[key] = None
    return out, pos


def loads(text: str) -> Any:
    lines = _lines(text)
    if not lines:
        return {}
    value, pos = _parse_block(lines, 0, lines[0].indent)
    if pos != len(lines):
        raise YamlError(f"line {lines[pos].no}: unexpected indentation")
    return value


def load(path: str) -> Any:
    with open(path, "r", encoding="utf-8") as fh:
        return loads(fh.read())
