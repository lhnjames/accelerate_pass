"""Generate one self-contained OpenEvolve input directory per PolyBench kernel.

Each program dir contains:
  initial_program.c  -- the untouched kernel .c with EVOLVE-BLOCK markers placed
                        around ONLY the kernel_<name> function (so OpenEvolve
                        evolves the same code region COMET rewrites, nothing else)
  evaluator.py       -- a copy of the shared, correctness-gated evaluator
  eval_config.json   -- per-program absolute paths + measurement parameters
  config.yaml        -- LLM (same DeepSeek model as COMET) + iteration budget

The EVOLVE region is located with COMET's own extract_kernel_function() so the
two systems operate on an identical definition of "the kernel".
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT))

from tune_source import extract_kernel_function

HERE = Path(__file__).resolve().parent
EVALUATOR_SRC = HERE / "evaluator.py"

MARK_START = "/* EVOLVE-BLOCK-START */"
MARK_END = "/* EVOLVE-BLOCK-END */"

# The 30 PolyBench/C 4.2 kernels, kernel entry-function name per program.
PROGRAMS = {
    "correlation": "datamining/correlation/correlation.c",
    "covariance": "datamining/covariance/covariance.c",
    "gemm": "linear-algebra/blas/gemm/gemm.c",
    "gemver": "linear-algebra/blas/gemver/gemver.c",
    "gesummv": "linear-algebra/blas/gesummv/gesummv.c",
    "symm": "linear-algebra/blas/symm/symm.c",
    "syr2k": "linear-algebra/blas/syr2k/syr2k.c",
    "syrk": "linear-algebra/blas/syrk/syrk.c",
    "trmm": "linear-algebra/blas/trmm/trmm.c",
    "2mm": "linear-algebra/kernels/2mm/2mm.c",
    "3mm": "linear-algebra/kernels/3mm/3mm.c",
    "atax": "linear-algebra/kernels/atax/atax.c",
    "bicg": "linear-algebra/kernels/bicg/bicg.c",
    "doitgen": "linear-algebra/kernels/doitgen/doitgen.c",
    "mvt": "linear-algebra/kernels/mvt/mvt.c",
    "cholesky": "linear-algebra/solvers/cholesky/cholesky.c",
    "durbin": "linear-algebra/solvers/durbin/durbin.c",
    "gramschmidt": "linear-algebra/solvers/gramschmidt/gramschmidt.c",
    "ludcmp": "linear-algebra/solvers/ludcmp/ludcmp.c",
    "lu": "linear-algebra/solvers/lu/lu.c",
    "trisolv": "linear-algebra/solvers/trisolv/trisolv.c",
    "deriche": "medley/deriche/deriche.c",
    "floyd-warshall": "medley/floyd-warshall/floyd-warshall.c",
    "nussinov": "medley/nussinov/nussinov.c",
    "adi": "stencils/adi/adi.c",
    "fdtd-2d": "stencils/fdtd-2d/fdtd-2d.c",
    "heat-3d": "stencils/heat-3d/heat-3d.c",
    "jacobi-1d": "stencils/jacobi-1d/jacobi-1d.c",
    "jacobi-2d": "stencils/jacobi-2d/jacobi-2d.c",
    "seidel-2d": "stencils/seidel-2d/seidel-2d.c",
}

CONFIG_YAML = """\
# OpenEvolve config for PolyBench kernel `{stem}` -- matched to COMET.
# Iteration budget mirrors COMET's --rounds {rounds}; the LLM is the SAME model
# COMET uses (deepseek-v4-pro via api.deepseek.com) so the comparison isolates
# the SEARCH STRATEGY, not the model.
max_iterations: {rounds}
checkpoint_interval: {rounds}

language: c
file_suffix: ".c"
diff_based_evolution: true
max_code_length: 40000

llm:
  api_base: "{base_url}"
  api_key: "${{DEEPSEEK_API_KEY}}"
  primary_model: "{model}"
  primary_model_weight: 1.0
  temperature: {temperature}
  max_tokens: {max_tokens}
  timeout: 120

prompt:
  system_message: |
    You are an expert C performance engineer. You are given a PolyBench
    computational kernel compiled with clang -O3. Rewrite ONLY the code between
    the EVOLVE-BLOCK markers to make it run FASTER while producing numerically
    identical results (the harness recompiles at -O3 and rejects any version
    whose output differs). Preserve the function signature and semantics. Good
    strategies include loop interchange for cache locality, loop tiling/blocking,
    scalar replacement, and improving vectorizability. Do NOT change the
    algorithm's mathematical result.

database:
  population_size: 20
  num_islands: 1
  elite_selection_ratio: 0.3
  exploitation_ratio: 0.7

evaluator:
  timeout: {eval_timeout}
  parallel_evaluations: 1
"""


def _insert_markers(src_text: str, kernel_name: str) -> str:
    body, _, _ = extract_kernel_function(src_text, kernel_name)
    if not body or body not in src_text:
        raise ValueError(f"could not locate kernel body for {kernel_name}")
    if MARK_START in src_text:
        raise ValueError("markers already present")
    return src_text.replace(body, f"{MARK_START}\n{body}\n{MARK_END}", 1)


def generate(out_root: Path, polybench_root: Path, *, rounds: int,
             base_url: str, model: str, temperature: float, max_tokens: int,
             pin_cpu: int | None, clang: str, fitness_runs: int,
             eval_timeout: int, only: list[str] | None) -> list[str]:
    utilities = (polybench_root / "utilities").resolve()
    polybench_c = (utilities / "polybench.c").resolve()
    made = []
    for stem, rel in PROGRAMS.items():
        if only and stem not in only:
            continue
        src_c = (polybench_root / rel).resolve()
        if not src_c.exists():
            print(f"[skip] {stem}: {src_c} missing")
            continue
        kernel_name = f"kernel_{stem.replace('-', '_')}"
        text = src_c.read_text()
        try:
            marked = _insert_markers(text, kernel_name)
        except ValueError as exc:
            print(f"[skip] {stem}: {exc}")
            continue

        pdir = out_root / stem
        pdir.mkdir(parents=True, exist_ok=True)
        (pdir / "initial_program.c").write_text(marked)
        shutil.copy2(EVALUATOR_SRC, pdir / "evaluator.py")
        (pdir / "eval_config.json").write_text(json.dumps({
            "stem": stem,
            "clang": clang,
            "utilities_dir": str(utilities),
            "source_include_dir": str(src_c.parent),
            "reference_c": str(src_c),        # untouched original = correctness + baseline ref
            "polybench_c": str(polybench_c),
            "pin_cpu": pin_cpu,
            "dataset_define": "LARGE_DATASET",
            "fitness_runs": fitness_runs,
        }, indent=2))
        (pdir / "config.yaml").write_text(CONFIG_YAML.format(
            stem=stem, rounds=rounds, base_url=base_url, model=model,
            temperature=temperature, max_tokens=max_tokens,
            eval_timeout=eval_timeout))
        made.append(stem)
        print(f"[ok] {stem} -> {pdir}")
    return made


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out-root", default=str(PROJECT_ROOT / "openevolve_compare" / "programs"))
    ap.add_argument("--polybench-root", default=str(PROJECT_ROOT / "PolyBenchC_no_rag"))
    ap.add_argument("--rounds", type=int, default=3, help="== COMET --rounds")
    ap.add_argument("--base-url", default="https://api.deepseek.com")
    ap.add_argument("--model", default="deepseek-v4-pro")
    ap.add_argument("--temperature", type=float, default=0.6)
    ap.add_argument("--max-tokens", type=int, default=4000)
    ap.add_argument("--pin-cpu", type=int, default=2)
    ap.add_argument("--clang", default="/usr/bin/clang-21")
    ap.add_argument("--fitness-runs", type=int, default=3)
    ap.add_argument("--eval-timeout", type=int, default=600)
    ap.add_argument("--only", nargs="*", default=None)
    args = ap.parse_args()

    out_root = Path(args.out_root)
    out_root.mkdir(parents=True, exist_ok=True)
    made = generate(
        out_root, Path(args.polybench_root), rounds=args.rounds,
        base_url=args.base_url, model=args.model, temperature=args.temperature,
        max_tokens=args.max_tokens, pin_cpu=args.pin_cpu, clang=args.clang,
        fitness_runs=args.fitness_runs, eval_timeout=args.eval_timeout,
        only=args.only)
    print(f"\ngenerated {len(made)} program dirs under {out_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
