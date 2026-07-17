#!/usr/bin/env python3
"""
Generate one self-contained PolyBench-shaped kernel file per TSVC loop.

Each TSVC loop (s000, s111, s1111, ...) becomes:
  TSVC_shim/kernels/<loop>/<loop>.c

containing a `kernel_<loop>(struct args_t *)` function (the real loop body,
renamed, __func__ dispatch calls fixed to use the loop's real name) plus a
`main()` that follows the exact PolyBench convention (polybench_start/stop/
print_instruments for timing, POLYBENCH_DUMP_* for correctness via the loop's
checksum). This lets the existing COMET harness (compile_binary/run_timing/
_correctness_check/collect_all_evidence — none of it touched) treat every
TSVC loop exactly like a PolyBench kernel, because find_polybench_utilities
resolves TSVC_shim/utilities/ (a private copy of polybench.c/h with TSVC's
common.c + dummy.c + global array storage appended — see
TSVC_shim/utilities/polybench.c) for any kernel path under TSVC_shim/.

Does not modify optimize.py / tune_param.py / tune_source.py's algorithm.
"""
import re
from pathlib import Path

TSVC_SRC = Path("/home/hanning/tsvc/src/tsvc.c")
OUT_ROOT = Path("/home/hanning/comet/TSVC_shim/kernels")

MAIN_TEMPLATE = '''\
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <malloc.h>

#include <polybench.h>
#include "common.h"
#include "array_defs.h"

{kernel_body}

static void print_checksum(real_t chk)
{{
  POLYBENCH_DUMP_START;
  POLYBENCH_DUMP_BEGIN("checksum");
  fprintf(POLYBENCH_DUMP_TARGET, "%.6f", chk);
  POLYBENCH_DUMP_END("checksum");
  POLYBENCH_DUMP_FINISH;
}}

int main(int argc, char** argv)
{{
  int n1 = 1;
  int n3 = 1;
  int* ip;
  real_t s1, s2;
  init(&ip, &s1, &s2);

  struct args_t func_args = {{.arg_info = {arg_info}}};

  polybench_start_instruments;
  real_t chk = kernel_{loop}(&func_args);
  polybench_stop_instruments;
  polybench_print_instruments;

  polybench_prevent_dce(print_checksum(chk));

  free(ip);
  return 0;
}}
'''


def extract_function(text: str, name: str):
    """Brace-counting extraction of `<any-type> name(...) { ... }`.

    Anchored to the start of a line (mod leading whitespace) so a
    commented-out declaration like `//int s162(int k)` above the real
    `real_t s162(struct args_t * func_args)` definition can't be matched
    first -- `//` isn't `[ \\t]*`, so the regex skips straight past it to
    the real signature. Unanchored, re.search matched mid-comment (right
    after the `//`), which silently dropped the comment marker and spliced
    the stray commented-out signature onto the real one, producing a
    kernel file with two colliding signatures (a syntax error) instead of
    a clean skip.
    """
    pattern = r"^[ \t]*(?:static\s+)?[A-Za-z_][A-Za-z_0-9]*\s*\*?\s*" + re.escape(name) + r"\s*\("
    m = re.search(pattern, text, re.MULTILINE)
    if not m:
        return None
    brace_start = text.find("{", m.end())
    if brace_start == -1:
        return None
    brace, end = 0, -1
    for i in range(brace_start, len(text)):
        ch = text[i]
        if ch == "{":
            brace += 1
        elif ch == "}":
            brace -= 1
            if brace == 0:
                end = i + 1
                break
    if end == -1:
        return None
    return text[m.start():end]


def find_all_top_level_functions(text: str) -> dict:
    """Map every top-level function name defined (not just declared) in
    `text` to its full definition text, via the same anchored, brace-
    counting extraction as extract_function(). Used to pull in sibling
    helper functions that a loop calls but doesn't itself define -- e.g.
    s151 calls s151s(), s31111 calls test() -- which live as independent
    top-level functions elsewhere in tsvc.c, not inside the loop body."""
    pattern = re.compile(
        r"^[ \t]*(?:static\s+)?[A-Za-z_][A-Za-z_0-9]*\s*\*?\s*"
        r"([A-Za-z_][A-Za-z_0-9]*)\s*\([^;{]*\)\s*\{",
        re.MULTILINE,
    )
    out = {}
    for m in pattern.finditer(text):
        name = m.group(1)
        if name in out:
            continue
        body = extract_function(text, name)
        if body:
            out[name] = body
    return out


def main():
    text = TSVC_SRC.read_text()

    m = re.search(r"int main\(int argc, char \*\* argv\)\{(.*)", text, re.S)
    assert m, "could not find tsvc.c main()"
    calls = re.findall(r"time_function\(&(\w+),\s*(.*?)\);", m.group(1))
    assert len(calls) >= 100, f"expected ~150 loop calls, got {len(calls)}"
    loop_names = {c[0] for c in calls}
    all_funcs = find_all_top_level_functions(text)

    OUT_ROOT.mkdir(parents=True, exist_ok=True)
    generated = []
    skipped = []

    for loop, arg_info in calls:
        body = extract_function(text, loop)
        if body is None:
            skipped.append(loop)
            continue

        # Sibling helper functions the loop body calls (see
        # find_all_top_level_functions docstring) -- must be pulled in or
        # the kernel fails to link with "undefined reference". Exclude
        # other TSVC loop names so we never accidentally drag in a second,
        # unrelated ~150-line loop just because its name appears as a
        # substring match.
        called = set(re.findall(r"\b([A-Za-z_][A-Za-z_0-9]*)\s*\(", body))
        helpers = sorted(n for n in called
                         if n in all_funcs and n != loop and n not in loop_names)
        helper_text = "\n\n".join(all_funcs[h] for h in helpers)

        # Rename the function definition identifier only (first occurrence,
        # which is the signature) and fix __func__ dispatch calls so
        # initialise_arrays()/calc_checksum() still match the *real* loop
        # name (they strcmp against literal names like "s111", not
        # "kernel_s111").
        sig_pattern = r"([A-Za-z_][A-Za-z_0-9]*\s*\*?\s*)" + re.escape(loop) + r"(\s*\()"
        renamed, n = re.subn(sig_pattern, r"\1kernel_" + loop + r"\2", body, count=1)
        assert n == 1, f"failed to rename {loop}"

        full_body = (helper_text + "\n\n" + renamed) if helper_text else renamed
        full_body = full_body.replace("__func__", f'"{loop}"')

        kernel_dir = OUT_ROOT / loop
        kernel_dir.mkdir(parents=True, exist_ok=True)
        out_file = kernel_dir / f"{loop}.c"
        out_file.write_text(MAIN_TEMPLATE.format(
            kernel_body=full_body, loop=loop, arg_info=arg_info,
        ))
        generated.append(loop)

    print(f"generated {len(generated)} kernels under {OUT_ROOT}")
    if skipped:
        print(f"skipped {len(skipped)}: {skipped}")

    manifest = OUT_ROOT.parent / "manifest.txt"
    manifest.write_text("\n".join(str(OUT_ROOT / loop / f"{loop}.c") for loop in generated) + "\n")
    print(f"manifest written: {manifest} ({len(generated)} kernels)")


if __name__ == "__main__":
    main()
