"""Fail-closed validation for COMET's required LLVM 21 toolchain."""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Mapping


LLVM_VERSION_RE = re.compile(r"\bversion\s+(\d+)(?:\.(\d+))?")


@dataclass(frozen=True)
class ToolIdentity:
    role: str
    configured_path: str
    resolved_path: str
    version_line: str
    major: int


@dataclass(frozen=True)
class LLVM21Identity:
    tools: tuple[ToolIdentity, ...]
    identity_sha256: str

    def to_dict(self) -> dict:
        return {
            "tools": [asdict(tool) for tool in self.tools],
            "identity_sha256": self.identity_sha256,
        }


def parse_llvm_major(version_output: str) -> int:
    match = LLVM_VERSION_RE.search(version_output)
    if not match:
        raise ValueError(f"cannot parse LLVM version output: {version_output[:160]!r}")
    return int(match.group(1))


def _inspect_tool(role: str, configured_path: str, timeout: int) -> ToolIdentity:
    path = Path(configured_path).expanduser()
    if not path.is_file():
        raise RuntimeError(f"required LLVM 21 tool is missing: {role}={path}")
    if not path.stat().st_mode & 0o111:
        raise RuntimeError(f"required LLVM 21 tool is not executable: {role}={path}")
    try:
        proc = subprocess.run(
            [str(path), "--version"], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, errors="replace",
            timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(f"failed to inspect {role} at {path}: {exc}") from exc
    output = proc.stdout.strip()
    if proc.returncode != 0:
        raise RuntimeError(
            f"{role} --version exited {proc.returncode}: {output[:240]}")
    major = parse_llvm_major(output)
    if major != 21:
        raise RuntimeError(
            f"COMET requires LLVM 21, but {role} reports major {major}: "
            f"{output.splitlines()[0] if output else path}")
    return ToolIdentity(
        role=role,
        configured_path=str(path),
        resolved_path=str(path.resolve()),
        version_line=output.splitlines()[0],
        major=major,
    )


def verify_llvm21_toolchain(compiler_config, timeout: int = 10) -> LLVM21Identity:
    """Validate clang, clang++, opt, and llc and return auditable identity data."""
    configured: Mapping[str, str] = {
        "clang": compiler_config.clang_path,
        "clang++": compiler_config.clang_cxx_path,
        "opt": compiler_config.opt_path,
        "llc": compiler_config.llc_path,
    }
    tools = tuple(_inspect_tool(role, path, timeout)
                  for role, path in configured.items())
    canonical = json.dumps([asdict(tool) for tool in tools], sort_keys=True,
                           separators=(",", ":")).encode("utf-8")
    return LLVM21Identity(
        tools=tools,
        identity_sha256=hashlib.sha256(canonical).hexdigest())
