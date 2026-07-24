#!/usr/bin/env python3
"""Regenerate the legacy global option inventory from the pinned opt-21."""
from __future__ import annotations

import json
import re
import subprocess
from datetime import datetime, timezone
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "configs/config.yaml"
OUTPUT = ROOT / "configs/discovered_pass_parameters.json"


def main() -> None:
    config = yaml.safe_load(CONFIG.read_text(encoding="utf-8"))
    opt_path = config["compiler"]["opt_path"]
    version_run = subprocess.run(
        [opt_path, "--version"], capture_output=True, text=True,
        errors="replace", check=True, timeout=30)
    version = (version_run.stdout or version_run.stderr).splitlines()[0]
    if "version 21" not in version and "21." not in version:
        raise SystemExit(f"refusing to generate cache from non-LLVM-21 opt: {version}")

    help_run = subprocess.run(
        [opt_path, "--help-hidden"], capture_output=True, text=True,
        errors="replace", check=True, timeout=60)
    option_re = re.compile(
        r"^\s+--([A-Za-z0-9][A-Za-z0-9_.-]*)(?:=<([^>]+)>)?\s+-\s+(.+)$")
    options = []
    seen = set()
    for line in (help_run.stdout + help_run.stderr).splitlines():
        match = option_re.match(line)
        if not match or match.group(1) in seen:
            continue
        name, value_type, description = match.groups()
        seen.add(name)
        options.append({
            "description": description.strip(),
            "name": name,
            "value_type": value_type or "bool",
        })

    payload = {
        "opt_path": opt_path,
        "llvm_version": version,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "options": sorted(options, key=lambda item: item["name"]),
    }
    OUTPUT.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {len(options)} LLVM 21 options to {OUTPUT}")


if __name__ == "__main__":
    main()
