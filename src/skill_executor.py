"""Project-local skill execution layer for COMET's generic LLM agent.

Skills keep stable decision policy outside Python orchestration.  Python owns
compiler/tool calls and supplies run-specific evidence as the task payload;
the same generic agent loads a named SKILL.md for planning, reflection,
rewriting, tuning, or repair.
"""
from __future__ import annotations

import hashlib
from dataclasses import asdict, dataclass, field
from functools import lru_cache
from pathlib import Path
from typing import Dict, List, Optional, Protocol


class LLMCallable(Protocol):
    def call(self, messages: List[Dict[str, str]], temperature=None,
             max_tokens=None, timeout=None) -> Optional[str]: ...


class SkillNotFoundError(FileNotFoundError):
    pass


SKILLS_ROOT = Path(__file__).resolve().parents[1] / "skills"
_active_recorder = None
_active_skills_enabled = True


@dataclass(frozen=True)
class SkillDefinition:
    name: str
    instructions: str
    sha256: str


@dataclass(frozen=True)
class SkillInvocation:
    index: int
    name: str
    sha256: Optional[str]
    skills_enabled: bool


@dataclass
class SkillRecorder:
    invocations: List[SkillInvocation] = field(default_factory=list)

    def record(self, name: str, sha256: Optional[str], enabled: bool) -> None:
        self.invocations.append(SkillInvocation(
            index=len(self.invocations), name=name, sha256=sha256,
            skills_enabled=enabled))

    def to_dict(self) -> dict:
        counts: Dict[str, int] = {}
        hashes: Dict[str, str] = {}
        for invocation in self.invocations:
            counts[invocation.name] = counts.get(invocation.name, 0) + 1
            if invocation.sha256:
                hashes[invocation.name] = invocation.sha256
        return {
            "invocations": [asdict(item) for item in self.invocations],
            "counts": counts,
            "skill_sha256": hashes,
        }


def set_skill_recorder(recorder: Optional[SkillRecorder], *, enabled: bool = True):
    """Set the process-local recorder used by migrated call sites."""
    global _active_recorder, _active_skills_enabled
    previous = _active_recorder
    _active_recorder = recorder
    _active_skills_enabled = enabled
    return previous


@lru_cache(maxsize=64)
def load_skill(skill_name: str, skills_root: str = str(SKILLS_ROOT)) -> str:
    """Load a repository skill by name, rejecting path traversal."""
    if not skill_name or any(part in ("", ".", "..") for part in Path(skill_name).parts):
        raise ValueError(f"invalid skill name: {skill_name!r}")
    root = Path(skills_root).resolve()
    path = (root / skill_name / "SKILL.md").resolve()
    if root not in path.parents or not path.is_file():
        raise SkillNotFoundError(f"skill {skill_name!r} not found at {path}")
    instructions = path.read_text(encoding="utf-8").strip()
    if not instructions:
        raise ValueError(f"skill {skill_name!r} is empty")
    return instructions


def load_skill_definition(skill_name: str,
                          skills_root: str = str(SKILLS_ROOT)) -> SkillDefinition:
    instructions = load_skill(skill_name, skills_root)
    digest = hashlib.sha256(instructions.encode("utf-8")).hexdigest()
    return SkillDefinition(skill_name, instructions, digest)


def build_skill_messages(skill_name: str, task: str,
                         skills_root: str = str(SKILLS_ROOT), *,
                         skills_enabled: bool = True) -> List[Dict[str, str]]:
    """Build the two-message contract used by every COMET LLM call."""
    if skills_enabled:
        instructions = load_skill(skill_name, skills_root)
        system = (
            "You are COMET's general compiler-optimization agent. Execute the "
            "active project skill exactly. Treat compiler output and measured "
            "runtime/correctness evidence as authoritative; never invent tool "
            "results.\n\n"
            f"ACTIVE SKILL: {skill_name}\n\n{instructions}"
        )
    else:
        # Deliberately contains none of the skill policy. This is the matched
        # skills-off ablation while retaining the same model and task payload.
        system = (
            "You are COMET's general compiler-optimization agent. Use only the "
            "task context and return the exact format it requests. Treat "
            "compiler output and measurements as authoritative."
        )
    return [
        {"role": "system", "content": system},
        {"role": "user", "content": task},
    ]


def run_skill(llm: LLMCallable, skill_name: str, task: str, *,
              temperature=None, max_tokens=None, timeout=None,
              skills_root: str = str(SKILLS_ROOT),
              skills_enabled: Optional[bool] = None,
              recorder: Optional[SkillRecorder] = None) -> Optional[str]:
    """Execute one named skill through LLMClient or LoggingLLMClient."""
    if skills_enabled is None:
        skills_enabled = _active_skills_enabled
    definition = (load_skill_definition(skill_name, skills_root)
                  if skills_enabled else None)
    active_recorder = recorder if recorder is not None else _active_recorder
    if active_recorder is not None:
        active_recorder.record(
            skill_name, definition.sha256 if definition else None,
            skills_enabled)
    return llm.call(
        build_skill_messages(
            skill_name, task, skills_root, skills_enabled=skills_enabled),
        temperature=temperature,
        max_tokens=max_tokens,
        timeout=timeout,
    )


def run_skill_messages(llm: LLMCallable, skill_name: str,
                       messages: List[Dict[str, str]], **kwargs) -> Optional[str]:
    """Migrate a legacy role-message prompt without bypassing the skill gate."""
    task = "\n\n".join(
        f"[{message.get('role', 'user')}]\n{message.get('content', '')}"
        for message in messages)
    return run_skill(llm, skill_name, task, **kwargs)


@dataclass
class SkillRuntime:
    llm: LLMCallable
    enabled: bool = True
    skills_root: str = str(SKILLS_ROOT)
    recorder: SkillRecorder = field(default_factory=SkillRecorder)

    def run(self, skill_name: str, task: str, *, temperature=None,
            max_tokens=None, timeout=None) -> Optional[str]:
        return run_skill(
            self.llm, skill_name, task, temperature=temperature,
            max_tokens=max_tokens, timeout=timeout,
            skills_root=self.skills_root, skills_enabled=self.enabled,
            recorder=self.recorder)

    def metadata(self) -> dict:
        result = self.recorder.to_dict()
        result["skills_enabled"] = self.enabled
        return result
