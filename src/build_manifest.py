"""Language-aware, non-unity build manifests for real C/C++ benchmarks."""
from __future__ import annotations

import hashlib
import json
import os
import signal
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence

from src.build_utils import is_cxx_source


@dataclass(frozen=True)
class TranslationUnit:
    path: Path
    language: str
    flags: tuple[str, ...] = ()


@dataclass(frozen=True)
class RuntimeContract:
    cwd: Path
    argv: tuple[str, ...] = ()
    stdin: Optional[Path] = None
    output_files: tuple[Path, ...] = ()


@dataclass(frozen=True)
class BuildManifest:
    name: str
    manifest_path: Path
    sources: tuple[TranslationUnit, ...]
    include_dirs: tuple[Path, ...] = ()
    defines: tuple[str, ...] = ()
    compile_flags: tuple[str, ...] = ()
    link_flags: tuple[str, ...] = ()
    c_standard: str = "gnu99"
    cxx_standard: str = "gnu++17"
    runtime: Optional[RuntimeContract] = None
    workloads: tuple[tuple[str, RuntimeContract], ...] = ()
    default_workload: Optional[str] = None

    @property
    def uses_cxx(self) -> bool:
        return any(unit.language == "c++" for unit in self.sources)

    def runtime_for(self, workload: Optional[str] = None) -> RuntimeContract:
        """Resolve a named workload, or the manifest's default runtime.

        ``runtime`` remains supported for version-1 manifests created before
        named workloads were introduced.  New SPEC manifests carry both the
        test and reference contracts and identify which is the default.
        """
        named = dict(self.workloads)
        selected = workload or self.default_workload
        if selected is not None:
            try:
                return named[selected]
            except KeyError as exc:
                raise ValueError(
                    f"unknown workload {selected!r}; available: {sorted(named)}"
                ) from exc
        if self.runtime is not None:
            return self.runtime
        if len(named) == 1:
            return next(iter(named.values()))
        raise ValueError("manifest has no unambiguous runtime contract")


@dataclass
class BuildResult:
    success: bool
    binary: Optional[Path] = None
    error: str = ""
    commands: List[List[str]] = field(default_factory=list)


def _resolve(base: Path, value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else (base / path).resolve()


def _source_language(path: Path, explicit: Optional[str]) -> str:
    if explicit:
        normalized = explicit.strip().lower()
        if normalized in ("c++", "cpp", "cxx"):
            return "c++"
        if normalized == "c":
            return "c"
        raise ValueError(f"unsupported source language {explicit!r} for {path}")
    return "c++" if is_cxx_source(path) else "c"


def _parse_runtime(base: Path, data: dict) -> RuntimeContract:
    argv = tuple(str(value) for value in data.get("argv", ()))
    if not argv:
        raise ValueError("runtime argv must include a logical argv[0]")
    return RuntimeContract(
        cwd=_resolve(base, data.get("cwd", ".")),
        argv=argv,
        stdin=(_resolve(base, data["stdin"]) if data.get("stdin") else None),
        output_files=tuple(_resolve(base, p)
                           for p in data.get("output_files", ())),
    )


def load_build_manifest(path: "str | Path") -> BuildManifest:
    manifest_path = Path(path).resolve()
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    if data.get("version") != 1:
        raise ValueError(f"unsupported build manifest version: {data.get('version')!r}")
    base = manifest_path.parent
    raw_sources = data.get("sources") or []
    if not raw_sources:
        raise ValueError("build manifest must contain at least one source")

    sources = []
    for item in raw_sources:
        if isinstance(item, str):
            source_path, language, flags = _resolve(base, item), None, ()
        else:
            source_path = _resolve(base, item["path"])
            language = item.get("language")
            flags = tuple(item.get("flags", ()))
        sources.append(TranslationUnit(
            path=source_path,
            language=_source_language(source_path, language),
            flags=flags,
        ))

    runtime_data = data.get("runtime")
    runtime = _parse_runtime(base, runtime_data) if runtime_data else None
    raw_workloads = data.get("workloads") or {}
    if not isinstance(raw_workloads, dict):
        raise ValueError("manifest workloads must be an object keyed by name")
    workloads = tuple(
        (str(name), _parse_runtime(base, workload_data))
        for name, workload_data in raw_workloads.items()
    )
    default_workload = data.get("default_workload")
    if default_workload is not None and default_workload not in raw_workloads:
        raise ValueError(
            f"default_workload {default_workload!r} is not present in workloads")

    return BuildManifest(
        name=data["name"],
        manifest_path=manifest_path,
        sources=tuple(sources),
        include_dirs=tuple(_resolve(base, p) for p in data.get("include_dirs", ())),
        defines=tuple(data.get("defines", ())),
        compile_flags=tuple(data.get("compile_flags", ())),
        link_flags=tuple(data.get("link_flags", ())),
        c_standard=data.get("c_standard", "gnu99"),
        cxx_standard=data.get("cxx_standard", "gnu++17"),
        runtime=runtime,
        workloads=workloads,
        default_workload=default_workload,
    )


class MultiTUBuilder:
    """Compile each translation unit independently, then link once.

    This intentionally never concatenates sources. It preserves C++ namespace,
    template, internal-linkage, and ODR boundaries that unity builds destroy.
    """

    def __init__(self, compiler_config):
        self.compiler = compiler_config

    @staticmethod
    def _run(cmd: Sequence[str], timeout: int, cwd: Path) -> tuple[bool, str]:
        try:
            proc = subprocess.Popen(
                list(cmd), cwd=str(cwd), stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, errors="replace",
                start_new_session=True)
        except OSError as exc:
            return False, str(exc)
        try:
            _, stderr = proc.communicate(timeout=timeout)
            return proc.returncode == 0, stderr
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.wait()
            return False, f"command timed out after {timeout}s: {' '.join(cmd)}"

    def build(self, manifest: BuildManifest, output: "str | Path",
              build_dir: "str | Path", extra_flags: Sequence[str] = (),
              source_overrides: Optional[Dict[Path, Path]] = None) -> BuildResult:
        output = Path(output).resolve()
        build_dir = Path(build_dir).resolve()
        build_dir.mkdir(parents=True, exist_ok=True)
        output.parent.mkdir(parents=True, exist_ok=True)
        source_overrides = {Path(k).resolve(): Path(v).resolve()
                            for k, v in (source_overrides or {}).items()}
        include_flags = [f"-I{path}" for path in manifest.include_dirs]
        define_flags = [d if d.startswith("-D") else f"-D{d}"
                        for d in manifest.defines]
        objects: List[Path] = []
        commands: List[List[str]] = []
        timeout = int(self.compiler.timeout_seconds)

        for unit in manifest.sources:
            actual_source = source_overrides.get(unit.path.resolve(), unit.path)
            if not actual_source.is_file():
                return BuildResult(False, error=f"source not found: {actual_source}",
                                   commands=commands)
            compiler = (self.compiler.clang_cxx_path if unit.language == "c++"
                        else self.compiler.clang_path)
            standard = (manifest.cxx_standard if unit.language == "c++"
                        else manifest.c_standard)
            identity = hashlib.sha256(str(unit.path).encode()).hexdigest()[:12]
            obj = build_dir / f"{unit.path.stem}-{identity}.o"
            cmd = [compiler, "-O3", f"-std={standard}", "-c", str(actual_source),
                   "-o", str(obj)]
            cmd += include_flags + define_flags + list(manifest.compile_flags)
            cmd += list(unit.flags) + list(extra_flags)
            commands.append(cmd)
            ok, error = self._run(cmd, timeout, manifest.manifest_path.parent)
            if not ok:
                return BuildResult(False, error=error, commands=commands)
            objects.append(obj)

        linker = (self.compiler.clang_cxx_path if manifest.uses_cxx
                  else self.compiler.clang_path)
        link_cmd = [linker, *map(str, objects), "-o", str(output),
                    *manifest.link_flags]
        commands.append(link_cmd)
        ok, error = self._run(link_cmd, timeout, manifest.manifest_path.parent)
        if not ok:
            return BuildResult(False, error=error, commands=commands)
        return BuildResult(True, binary=output, commands=commands)
