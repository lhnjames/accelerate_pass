# Prompt-to-skill migration map

Target invariant: orchestration code never calls `llm.call()` directly. Every
model decision executes through `src.skill_executor.run_skill()`, which loads a
versioned project `SKILL.md`. `src/llm_client.py` remains the transport and
`src/run_logger.py` remains a transparent logging wrapper.

| Existing flow | Current call site(s) | Project skill |
|---|---|---|
| unified next-action decision | `optimize.py::run_agent_step` | `action-decision` |
| short action sequence | `optimize.py::plan_action_sequence` | `meta-planning` |
| failed/non-improving result diagnosis | `optimize.py::reflect_on_failure` | `failure-reflection` |
| hotspot/source diagnosis | `optimize.py::_analyze_rewrite`, `tune_source.py::analyze_kernel_patterns` | `rewrite-analysis` |
| single/multi function implementation | unified and legacy source rounds | `source-rewrite` |
| numeric/hash behavior repair | unified and legacy precision-fix calls | `precision-repair` |
| compiler diagnostic repair | unified compile-fix call | `compile-repair` |
| complete pass/runtime audit before parameter debugging | `optimize.py::run_pass_runtime_analysis` | `pass-analysis` |
| initial and refinement flag selection | unified/legacy parameter rounds | `parameter-tuning` |

## Migration gate

The migration is wired: an AST-based test fails if a direct `.call(...)` on a name
`llm`, `_llm`, or `client` appears in `optimize.py`, `tune_param.py`, or
`tune_source.py`. It must also assert that every required skill is loaded by at
least one call site. The transport health check is not a compiler decision and
remains exempt.

Use `optimize.py --skills-off` for the matched generic-model ablation. The
default path loads the versioned `skills/*/SKILL.md` policy; migrated calls go
through `src.skill_executor.run_skill_messages()`, and the run logger records
ordered invocation names, enabled/disabled status, and SHA-256 skill hashes.

## Experimental versioning

Each run result must record:

- SHA-256 of every loaded `SKILL.md`;
- model identifier, temperature, and call budget;
- ordered skill invocation names and call count;
- token usage when exposed by the provider;
- whether a fallback parser or fallback action was used.

This metadata makes the skills-vs-no-skills ablation auditable and prevents a
changed skill text from being compared as if it were the same treatment.
