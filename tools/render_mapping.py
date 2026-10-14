#!/usr/bin/env python3
"""Render the DASH → OPI object mapping table from mapping/dash-opi.yaml."""

from __future__ import annotations

import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
YAML_PATH = ROOT / "mapping" / "dash-opi.yaml"


def parse_objects(text: str) -> list[dict[str, str]]:
    """Parse the objects: list without requiring PyYAML."""
    objects: list[dict[str, str]] = []
    current: dict[str, str] | None = None
    in_objects = False
    in_messages = False
    in_grpc = False
    messages: list[str] = []
    rpcs: list[str] = []

    def flush() -> None:
        nonlocal current, messages, rpcs, in_messages, in_grpc
        if current is not None:
            current["opi_messages"] = ", ".join(messages)
            current["grpc"] = ", ".join(rpcs)
            objects.append(current)
        current = None
        messages = []
        rpcs = []
        in_messages = False
        in_grpc = False

    for raw in text.splitlines():
        line = raw.rstrip()
        if line.startswith("objects:"):
            in_objects = True
            continue
        if not in_objects:
            continue
        if line.startswith("  - dash_object:"):
            flush()
            current = {"dash_object": line.split(":", 1)[1].strip()}
            continue
        if current is None:
            continue
        if line.startswith("    dash_sai_type:"):
            current["dash_sai_type"] = line.split(":", 1)[1].strip()
        elif line.startswith("    dash_entry_type:"):
            extra = line.split(":", 1)[1].strip()
            current["dash_sai_type"] += f", {extra}"
        elif line.startswith("    dash_header:"):
            current["dash_header"] = line.split(":", 1)[1].strip()
        elif line.startswith("    opi_proto:"):
            current["opi_proto"] = line.split(":", 1)[1].strip()
        elif line.startswith("    opi_messages:"):
            in_messages = True
            in_grpc = False
        elif line.startswith("    grpc:"):
            in_messages = False
            in_grpc = True
        elif line.startswith("    related:") or line.startswith("    attributes:") or line.startswith("    actions:"):
            in_messages = False
            in_grpc = False
        elif in_messages and line.startswith("      - "):
            messages.append(line.split("-", 1)[1].strip())
        elif in_grpc and line.startswith("      - "):
            rpcs.append(line.split("-", 1)[1].strip())

    flush()
    return objects


def markdown_table(objects: list[dict[str, str]]) -> str:
    headers = [
        "DASH object",
        "DASH SAI type / header",
        "OPI protobuf messages",
        "OPI proto file",
        "CloudInfraService RPCs",
    ]
    rows = []
    for obj in objects:
        sai = f"`{obj['dash_sai_type']}` in `{obj['dash_header']}`"
        rows.append(
            [
                f"**{obj['dash_object']}**",
                sai,
                ", ".join(f"`{m}`" for m in obj["opi_messages"].split(", ")),
                f"`{obj['opi_proto']}`",
                ", ".join(f"`{r}`" for r in obj["grpc"].split(", ")),
            ]
        )
    out = ["| " + " | ".join(headers) + " |", "| " + " | ".join("---" for _ in headers) + " |"]
    for row in rows:
        out.append("| " + " | ".join(row) + " |")
    return "\n".join(out)


def main() -> None:
    objects = parse_objects(YAML_PATH.read_text())
    print(markdown_table(objects))


if __name__ == "__main__":
    main()
