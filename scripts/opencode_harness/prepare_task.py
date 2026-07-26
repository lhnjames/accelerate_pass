#!/usr/bin/env python3
"""Set up an OpenCode baseline scratch dir for one comet benchmark program.

Usage: prepare_task.py <program_path relative to /home/hanning/comet> <scratch_dir>

Writes into scratch_dir: kernel.c (+ any sibling headers), measure.sh,
prompt.txt, baseline.json (baseline_ms, correctness_mode, ref_bin path).
"""
import sys, os, shutil, json, stat
from pathlib import Path

sys.path.insert(0, "/home/hanning/comet/scripts/opencode_harness")
from measure_lib import compile_and_time, correctness_check  # noqa: E402

COMET_ROOT = Path("/home/hanning/comet")
sys.path.insert(0, str(COMET_ROOT))
from src.polybench_paths import find_polybench_utilities  # noqa: E402
from src.correctness import detect_correctness_mode        # noqa: E402


def main():
    program_rel, scratch_dir = sys.argv[1], Path(sys.argv[2])
    pin_cpu = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] else None
    program_path = COMET_ROOT / program_rel
    scratch_dir.mkdir(parents=True, exist_ok=True)

    utils = find_polybench_utilities(str(program_path))
    if utils is None:
        sys.exit(f"could not resolve utilities/ dir for {program_path}")
    source_dir = program_path.parent

    # Copy every sibling file (headers, LICENSE, etc.) so opencode can see
    # everything the kernel #includes, then it edits kernel.c in place.
    for f in source_dir.iterdir():
        if f.is_file():
            dest_name = "kernel.c" if f.name == program_path.name else f.name
            shutil.copy2(f, scratch_dir / dest_name)

    # Reference binary + baseline timing (runs=3, same rigor as the final
    # confirmation) computed ONCE, up front, against the untouched original.
    ref_bin = scratch_dir / "ref_bin"
    ok, ms, err = compile_and_time(str(scratch_dir / "kernel.c"), str(utils),
                                    str(scratch_dir), runs=3, out_bin=str(ref_bin),
                                    pin_cpu=pin_cpu)
    if not ok:
        sys.exit(f"baseline compile/time failed: {err}")

    mode = detect_correctness_mode(str(ref_bin))

    (scratch_dir / "baseline.json").write_text(json.dumps({
        "program": program_rel,
        "utils": str(utils),
        "source_dir": str(scratch_dir),   # after edits, source_dir IS the scratch dir
        "baseline_ms": ms,
        "correctness_mode": mode,
        "ref_bin": str(ref_bin),
        "pin_cpu": pin_cpu,
    }, indent=1))

    measure_sh = scratch_dir / "measure.sh"
    pin_arg = str(pin_cpu) if pin_cpu is not None else ""
    measure_sh.write_text(
        "#!/bin/sh\n"
        f'exec /home/hanning/comet/.venv/bin/python3 /home/hanning/comet/scripts/opencode_harness/measure_cli.py "{utils}" "{scratch_dir}" {ms} {pin_arg}\n'
    )
    measure_sh.chmod(measure_sh.stat().st_mode | stat.S_IEXEC)

    kernel_name = program_path.stem.replace("-", "_")
    prompt = f"""You are optimizing a single C file, kernel.c, for RUNTIME SPEED on an aarch64 Linux machine, compiled with clang -O3 (LLVM 21).

Rules:
- Edit kernel.c directly. Do not rename the main computational function (`kernel_{kernel_name}` or similar -- check the file for the exact name) or change its signature.
- Preserve numerical/functional correctness -- this will be checked automatically against the original behavior after you're done. A faster but wrong answer scores zero.
- You do NOT get compiler flags, compiler diagnostics, or profiler output -- only your own reading of the code and whatever you measure with ./measure.sh.
- Run `./measure.sh` from this directory any time to compile+run your current kernel.c and see its speedup vs the -O3 baseline ({ms:.3f} ms). It is a single noisy timing run (like a quick sanity check), not a final verdict.
- You have a budget of roughly 9 optimization attempts (edit + measure), similar to what a compiler-tuning agent baseline gets. Stop earlier if you're confident you've converged (repeated attempts are not improving), or keep going if measure.sh keeps showing gains.
- Think about what actually limits this kernel's speed (memory access pattern, redundant recomputation, missed vectorization due to aliasing, loop structure, algorithmic redundancy) and rewrite the C code accordingly. You may use `restrict`, loop transformations, blocking/tiling, precomputing invariants, etc. -- anything expressible in portable C that -O3 can then compile.

When you believe you're done, stop -- do not add commentary after your last edit, the file's final on-disk state is what gets evaluated.
"""
    (scratch_dir / "prompt.txt").write_text(prompt)
    print(f"prepared {scratch_dir}: baseline={ms:.3f}ms mode={mode} utils={utils}")


if __name__ == "__main__":
    main()
