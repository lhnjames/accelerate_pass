#!/usr/bin/env python3
"""
O3 threshold parameter tuner — structured, evidence-based, no hardcoded params.

Design principles:
  1. Passes loaded dynamically from -debug-pass-manager (not hardcoded)
  2. Pass parameters discovered from opt --help-hidden (not hardcoded)
  3. Remarks filtered to KERNEL function line range (not IO/print code)
  4. LLM sees IR stats before/after each flag change (informed selection)
  5. Adaptive search: independent flags first, then joint top-2 (O(n·k) not O(k^n))

Flow:
  1. Find kernel function line range in source
  2. Collect -Rpass-missed remarks, filtered to kernel lines only
  3. Extract O3 passes that ran on kernel via -debug-pass-manager
  4. Discover tunable options from opt --help-hidden, keyed by pass name
  5. Score passes: kernel_missed_count × category_weight
  6. For top passes: run O3 with candidate flag values, show LLM IR stats diff
  7. LLM receives: scored table + missed reasons + IR stats diffs → picks ≤3 flags
  8. Adaptive grid search: independent then joint
  9. Report best and save JSON
"""
import os, sys, re, argparse, subprocess, tempfile, itertools, json
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, os.path.abspath(os.path.dirname(__file__)))
from src.config import ConfigLoader
from src.compiler_manager import CompilerRunner
from src.llm_client import LLMClient
from src.polybench_paths import find_polybench_utilities
from src.build_utils import run_timing, compile_c
from src.remarks import (REMARK_PASS_MAP, make_noinline_src,
                         extract_rich_remarks_yaml,
                         format_rich_remarks_for_source_prompt as format_rich_remark_for_prompt)


# ── Pass → keyword mapping for help-hidden search ────────────────────────────

## Pass → keyword overrides
# Needed only when flag prefix differs from what auto-derivation produces.
# auto-derivation: LICMPass→"licm", GVN→"gvn", JumpThreadingPass→"jump-threading" (all correct)
# Overrides: SLPVectorizer→"slp" (not "slpvectorizer"), LoopUnroll→"unroll" (not "loop-unroll"),
#            Inliner→"inline" (not "inliner"), SpeculativeExecution→"spec-exec"
PASS_KEYWORDS: dict = {
    "SLPVectorizerPass":          ["slp"],
    "LoopVectorizePass":          ["vectorize", "vectorizer-min"],
    "LoopUnrollPass":             ["unroll"],
    "LoopFullUnrollPass":         ["unroll"],
    "SimpleLoopUnswitchPass":     ["unswitch"],        # flags: unswitch-*, loop-unswitch-*
    "CallSiteSplittingPass":      ["callsite-splitting"],  # flag uses "callsite", not "call-site"
    "InlinerPass":                ["inline"],
    "SpeculativeExecutionPass":   ["spec-exec"],
    "MemCpyOptPass":              ["memcpy", "max-store-mem", "memset"],
    "AggressiveInstCombinePass":  ["instcombine"],
}


def _pass_name_to_keywords(pass_name: str) -> list:
    """Auto-derive search keywords from an LLVM pass class name.

    Uses ONLY the full lowercased name and kebab-case — deliberately does NOT
    add generic last-word suffixes like 'combine', 'simplify', 'opt', 'int',
    'promotion' which cause false-positive flag matches across unrelated passes.
    PASS_KEYWORDS overrides this for passes where the flag prefix differs.
    """
    import re as _re
    name = _re.sub(r"(Pass|Analysis|Transform|Impl|Legacy|WrapperPass)$", "", pass_name)
    kws = [name.lower()]
    kebab = _re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1-\2", name)
    kebab = _re.sub(r"([a-z\d])([A-Z])", r"\1-\2", kebab).lower()
    if kebab != name.lower():
        kws.append(kebab)
    return list(dict.fromkeys(kws))

PASS_CATEGORIES = {
    # Vectorization (highest impact for compute-bound kernels)
    "SLPVectorizerPass":          ("vectorize", 10),
    "LoopVectorizePass":          ("vectorize", 10),
    "VectorCombinePass":          ("vectorize",  7),
    # Loop transforms
    "LoopUnrollPass":             ("loop",       8),
    "LoopFullUnrollPass":         ("loop",       8),
    "SimpleLoopUnswitchPass":     ("loop",       7),
    "LoopDistributePass":         ("loop",       6),
    "LoopLoadEliminationPass":    ("memory",     7),
    "LoopSinkPass":               ("loop",       4),
    "LoopRotatePass":             ("loop",       3),
    # Memory / aliasing
    "LICMPass":                   ("memory",     6),
    "DSEPass":                    ("memory",     4),
    "MemCpyOptPass":              ("memory",     3),
    "MergedLoadStoreMotionPass":  ("memory",     4),
    # Scalar / redundancy elimination
    "GVN":                        ("scalar",     5),
    "EarlyCSEPass":               ("scalar",     4),
    "InstCombinePass":            ("scalar",     5),
    "AggressiveInstCombinePass":  ("scalar",     4),
    "ReassociatePass":            ("scalar",     3),
    "SCCPPass":                   ("scalar",     3),
    "SROAPass":                   ("scalar",     4),
    "IndVarSimplifyPass":         ("scalar",     3),
    # Control flow
    "JumpThreadingPass":          ("scalar",     4),
    "SimplifyCFGPass":            ("scalar",     3),
    "SpeculativeExecutionPass":   ("scalar",     4),
    "CallSiteSplittingPass":      ("scalar",     3),
    # Inlining
    "InlinerPass":                ("inline",     5),
}


def _pass_category(pass_name: str) -> tuple:
    """Return (category, weight) for a pass; defaults to ('other', 2) for unknowns."""
    return PASS_CATEGORIES.get(pass_name, ("other", 2))

# Flags that OVERRIDE the cost model (bypass O3 decisions) — always blacklisted
BLACKLISTED_FLAGS = {
    "force-vector-width", "force-vector-interleave",
    "force-target-max-vector-interleave",
    "unroll-count",                  # forces a specific count regardless of cost
    "unroll-full-max-count",
    "unroll-and-jam-count",
    "disable-loop-unrolling",
    "disable-inlining",
    "disable-licm-promotion",
    "force-attribute",
    "force-remove-attribute",
    "scalable-vectorization",        # arch-specific override
    # Debug/internal flags that affect correctness not performance
    "agg-antidep-debugdiv",
    "agg-antidep-debugmod",
    "instcombine-infinite-loop-threshold",
    "nvptx-fma-level",
}


# ── Helpers: discovery ────────────────────────────────────────────────────────

def get_kernel_line_range(src: str, kernel_name: str) -> tuple:
    """Return (first_line, last_line) of kernel function body (1-indexed)."""
    with open(src) as f:
        lines = f.readlines()
    brace_count, in_func, start = 0, False, None
    for i, line in enumerate(lines, 1):
        if start is None and kernel_name in line:
            start = i
        if start is not None:
            opens  = line.count('{')
            closes = line.count('}')
            if not in_func and opens > 0:
                in_func = True
            brace_count += opens - closes
            if in_func and brace_count <= 0:
                return start, i
    return (1, len(lines))


# Target-specific flag prefixes to exclude (never relevant to HPC kernels)
_TARGET_PREFIXES = frozenset({
    "aarch64", "amdgpu", "arm-", "asan", "arc-", "avr", "bpf", "hexagon",
    "lanai", "m68k", "mips", "msan", "nvptx", "ppc", "riscv", "sparc",
    "systemz", "thumb", "tsan", "ubsan", "lsan", "hwasan", "dfsan",
    "wasm", "x86-", "xcore", "ve-", "polly", "sancov",
})


def _flag_is_target_specific(flag: str) -> bool:
    fl = flag.lstrip("-")
    return any(fl.startswith(pfx) for pfx in _TARGET_PREFIXES)


def discover_options_from_help(opt_path: str, pass_names: list) -> dict:
    """
    Query opt --help-hidden and return {pass_name: [{"flag", "type", "desc"}]}.
    Only returns numeric options (=<int> or =<uint>) that are NOT blacklisted
    and NOT target-architecture-specific.

    Keywords are taken from PASS_KEYWORDS if the pass is listed there;
    otherwise they are auto-derived from the pass class name via
    _pass_name_to_keywords().  This covers ALL passes in the O3 pipeline,
    not just the ~13 that were previously hardcoded.
    """
    try:
        r = subprocess.run([opt_path, "--help-hidden"],
                           capture_output=True, text=True,
                           timeout=30, errors="replace")
        output = r.stdout + r.stderr
    except Exception:
        return {}

    opt_re = re.compile(r"^\s+--([a-z][a-z0-9-]*)(?:=<([^>]+)>)?\s+-\s+(.+)", re.I)
    all_opts = []
    for line in output.splitlines():
        m = opt_re.match(line)
        if not m:
            continue
        flag, typ, desc = m.group(1), m.group(2) or "bool", m.group(3)
        if flag in BLACKLISTED_FLAGS:
            continue
        if typ.lower() not in ("int", "uint", "number"):
            continue
        if _flag_is_target_specific(flag):
            continue
        all_opts.append({"flag": f"--{flag}", "type": typ, "desc": desc.strip()})

    result = {}
    for pname in pass_names:
        # Use explicit override if present, else auto-derive from class name
        keywords = PASS_KEYWORDS.get(pname) or _pass_name_to_keywords(pname)
        matched = []
        seen_flags: set = set()
        for opt in all_opts:
            flag_lower = opt["flag"].lower()
            if any(kw in flag_lower for kw in keywords):
                if flag_lower not in seen_flags:
                    seen_flags.add(flag_lower)
                    matched.append(opt)
        if matched:
            result[pname] = matched
    return result


def get_cpu_info() -> str:
    lines = []
    try:
        with open("/proc/cpuinfo") as f:
            cpuinfo = f.read()
        m = re.search(r"model name\s*:\s*(.+)", cpuinfo)
        if m:
            lines.append(f"CPU: {m.group(1).strip()}")
        fm = re.search(r"^flags\s*:\s*(.+)", cpuinfo, re.MULTILINE)
        if fm:
            flags = set(fm.group(1).split())
            if "avx512f" in flags:
                lines.append("SIMD: AVX-512 (8 doubles/vector, 512-bit)")
            elif "avx2" in flags:
                lines.append("SIMD: AVX2   (4 doubles/vector, 256-bit)")
    except Exception:
        pass
    try:
        r = subprocess.run(["lscpu"], capture_output=True, text=True, timeout=5)
        if r.returncode == 0:
            for ln in r.stdout.splitlines():
                if any(k in ln for k in ("L1d", "L2 ", "L3 ")):
                    lines.append(ln.strip())
    except Exception:
        pass
    return "\n".join(lines) if lines else "CPU info unavailable"


# ── Helpers: remarks + IR ─────────────────────────────────────────────────────

def extract_remarks_by_pass(clang: str, src: str, utils: Path,
                             source_dir: Path) -> dict:
    """
    Compile with -Rpass-missed=.* and parse into {opt_pass_name: [entries]}.
    Each entry: {"file", "line", "msg", "type"}.
    """
    polybench_c = utils / "polybench.c"
    cmd = [
        clang, "-O3", "-std=c99",
        f"-I{utils}", f"-I{source_dir}",
        "-DLARGE_DATASET", "-DPOLYBENCH_TIME",
        "-Rpass=.*", "-Rpass-missed=.*", "-Rpass-analysis=.*",
        src, str(polybench_c), "-o", "/dev/null", "-lm",
    ]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120,
                       errors="replace")
    pattern = re.compile(
        r"([^\s:]+\.c):(\d+):\d+: remark: (.+?)\s+\[-R(pass(?:-missed|-analysis)?)=([\w.-]+)\]"
    )
    by_pass: dict = defaultdict(list)
    for line in r.stderr.splitlines():
        m = pattern.search(line)
        if not m:
            continue
        fpath, lineno, msg, rtype, rpass = m.groups()
        opt_name = REMARK_PASS_MAP.get(rpass, rpass)
        entry = {
            "file":  os.path.basename(fpath),
            "line":  int(lineno),
            "msg":   msg.strip(),
            "type":  ("missed" if "missed" in rtype
                      else "analysis" if "analysis" in rtype
                      else "passed"),
        }
        by_pass[opt_name].append(entry)
    return dict(by_pass)


def filter_remarks_to_kernel(remarks_by_pass: dict,
                              kernel_start: int, kernel_end: int) -> dict:
    """
    Keep only remarks whose source line falls within the kernel function body.
    This removes IO/print/library remarks from the analysis.
    """
    filtered = {}
    for pname, entries in remarks_by_pass.items():
        kernel_entries = [
            e for e in entries
            if kernel_start <= e["line"] <= kernel_end
        ]
        if kernel_entries:
            filtered[pname] = kernel_entries
    return filtered


def _gen_o3_ir(clang: str, src: str, utils: "Path", source_dir: "Path",
               kernel_name: str, extra_mllvm: list = None) -> "str | None":
    """Compile noinline-patched source to O3 IR. Returns IR path or None."""
    tmp_src = make_noinline_src(src, kernel_name)
    fd, out_ir = tempfile.mkstemp(suffix=".ll")
    os.close(fd)
    cmd = [
        clang, "-O3", "-S", "-emit-llvm", "-std=c99",
        f"-I{utils}", f"-I{source_dir}",
        "-DLARGE_DATASET", "-DPOLYBENCH_TIME",
        tmp_src, "-o", out_ir,
    ]
    for f in (extra_mllvm or []):
        cmd += ["-mllvm", f]
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=60)
        return out_ir if r.returncode == 0 else None
    except Exception:
        return None
    finally:
        try:
            os.unlink(tmp_src)
        except Exception:
            pass


def _normalize_pass_name(raw: str) -> str:
    """LLVM 11's new pass manager prints loop/CGSCC-scoped passes wrapped in
    an adaptor, e.g. `FunctionToLoopPassAdaptor<llvm::LICMPass>` instead of
    plain `LICMPass` -- every OTHER piece of this codebase (remarks parsing
    via REMARK_PASS_MAP, _BOTTLENECK_PASS_PRIORITY, PASS_CATEGORIES, this
    file's own PASS_KEYWORDS) keys passes by the bare class name, so a
    wrapped name here never matches anything downstream. Concretely:
    discover_options_from_help(opt, kernel_passes) is called with THIS
    function's output as pass_names -- if LICM shows up as the wrapped
    string, its keyword-derivation garbles into nonsense and it silently
    gets zero discovered params (observed live: SPEC lbm_r's evidence
    collection correctly named LICMPass the #1 priority pass with 22 missed
    remarks, yet not one -licm-* flag was ever a candidate, across every
    run of this kernel, because kernel_passes never actually contained the
    string "LICMPass" for discover_options_from_help to match against).
    Pulls the innermost `llvm::<Name>` identifier out of any adaptor
    wrapping; passes with no such wrapping (the common case) pass through
    unchanged. Nested sub-pipeline managers (e.g.
    `FunctionToLoopPassAdaptor<llvm::PassManager<llvm::Loop, ...>>`, which
    aren't a specific tunable pass at all) fall back to their last
    llvm::-qualified segment -- harmless since that won't match any real
    pass name in _BOTTLENECK_PASS_PRIORITY/PASS_KEYWORDS either way.
    """
    matches = re.findall(r"llvm::(\w+)", raw)
    return matches[-1] if matches else raw


def extract_o3_passes_for_kernel(opt: str, ir_path: str,
                                  kernel_name: str, timeout: int = 120) -> list:
    """Run opt -debug-pass-manager on the given IR to find passes that ran on kernel."""
    cmd = [opt, "-passes=default<O3>", "-debug-pass-manager", "-disable-output", ir_path]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           errors="replace")
    except Exception:
        return []
    target = f"on {kernel_name}"
    seen, passes = set(), []
    for line in r.stderr.splitlines():
        if "Running pass:" in line and target in line:
            m = re.search(r"Running pass:\s+(\S+)", line)
            if m:
                name = _normalize_pass_name(m.group(1))
                if name not in seen:
                    seen.add(name)
                    passes.append(name)
    return passes


def get_ir_stats(clang: str, src: str, utils: Path, source_dir: Path,
                 kernel_name: str, extra_mllvm: list = None) -> dict:
    """
    Compile with clang -O3 -mllvm <flags> -S -emit-llvm.
    To prevent the kernel from being inlined into main (which would hide it from IR),
    we write a temp copy with __attribute__((noinline)) prepended to the kernel definition.
    This keeps the same O3 + mllvm-flags as real compilation.
    Returns: {vector_ops, fmul, fadd, total_instr, phi_nodes, vec_widths}
    """
    fd_src, tmp_src = tempfile.mkstemp(suffix=".c"); os.close(fd_src)
    fd_ir,  out_ir  = tempfile.mkstemp(suffix=".ll"); os.close(fd_ir)
    try:
        # Patch the source: add __attribute__((noinline)) to kernel function
        with open(src, errors="replace") as f:
            code = f.read()
        patched = code.replace(
            f"void {kernel_name}(",
            f"__attribute__((noinline)) void {kernel_name}("
        )
        if patched == code:
            # Fallback: try without void in case signature differs
            import re as _re
            patched = _re.sub(
                r"(static\s+)?void\s+" + re.escape(kernel_name) + r"\s*\(",
                r"__attribute__((noinline)) void " + kernel_name + r"(",
                code, count=1
            )
        with open(tmp_src, "w") as f:
            f.write(patched)

        # clang -O3 with mllvm flags — same as real build
        cmd = [
            clang, "-O3", "-S", "-emit-llvm", "-std=c99",
            f"-I{utils}", f"-I{source_dir}",
            "-DLARGE_DATASET", "-DPOLYBENCH_TIME",
            tmp_src, "-o", out_ir,
        ]
        for f in (extra_mllvm or []):
            cmd += ["-mllvm", f]
        r = subprocess.run(cmd, capture_output=True, timeout=60, errors="replace")
        if r.returncode != 0:
            return {}

        with open(out_ir, errors="replace") as f:
            content = f.read()

        # Find kernel function
        m = re.search(r"define[^\n]*@" + re.escape(kernel_name) + r"[^\n]*\{", content)
        if not m:
            return {}
        brace, end = 0, len(content)
        for i in range(m.end() - 1, len(content)):
            if content[i] == "{":
                brace += 1
            elif content[i] == "}":
                brace -= 1
            if brace == 0:
                end = i + 1; break

        kernel_ir = content[m.start():end]
        lines = kernel_ir.splitlines()

        vector_ops   = sum(1 for l in lines if re.search(r"<\d+ x ", l))
        fmul_ops     = sum(1 for l in lines if "fmul" in l)
        fadd_ops     = sum(1 for l in lines if "fadd" in l)
        total_instr  = sum(1 for l in lines if "=" in l and not l.strip().startswith(";"))
        phi_nodes    = sum(1 for l in lines if l.strip().startswith("%") and "phi" in l)
        # Count distinct vector widths
        widths = set(re.findall(r"<(\d+) x ", kernel_ir))

        return {
            "vector_ops":  vector_ops,
            "fmul":        fmul_ops,
            "fadd":        fadd_ops,
            "total_instr": total_instr,
            "phi_nodes":   phi_nodes,    # proxy for loop count
            "vec_widths":  sorted(widths, key=int) if widths else [],
        }
    except Exception:
        return {}
    finally:
        for f in (tmp_src, out_ir):
            try:
                os.unlink(f)
            except Exception:
                pass


def extract_kernel_ir(ir_path: str, kernel_name: str, max_lines: int = 150) -> str:
    with open(ir_path, errors="replace") as f:
        content = f.read()
    m = re.search(r"define[^\n]*@" + re.escape(kernel_name) + r"[^\n]*\{", content)
    if not m:
        return "\n".join(content.splitlines()[:max_lines]) + "\n...(truncated)"
    brace, in_body, end = 0, False, len(content)
    for i in range(m.end() - 1, len(content)):
        ch = content[i]
        if ch == "{":
            brace += 1; in_body = True
        elif ch == "}":
            brace -= 1
        if in_body and brace == 0:
            end = i + 1; break
    lines = content[m.start():end].splitlines()
    if len(lines) > max_lines:
        lines = lines[:max_lines] + ["  ...(truncated)"]
    return "\n".join(lines)


# ── Helpers: scoring ──────────────────────────────────────────────────────────

def score_passes(kernel_passes: list, kernel_remarks: dict,
                 discovered_opts: dict) -> list:
    """
    Score each pass with known discoverable options.
    Score = category_weight × (1 + kernel_missed_count / 3)
    Returns sorted list: [(score, pass_name, category, missed_count, missed_entries)]
    """
    scored = []
    candidates = (set(kernel_passes) | set(kernel_remarks.keys())) & set(discovered_opts.keys())
    for pname in candidates:
        cat_name, cat_weight = _pass_category(pname)
        entries = kernel_remarks.get(pname, [])
        missed  = [e for e in entries if e["type"] == "missed"]
        score   = cat_weight * (1.0 + len(missed) / 3.0)
        scored.append((score, pname, cat_name, len(missed), missed))
    scored.sort(reverse=True, key=lambda x: x[0])
    return scored


# ── Helpers: timing ───────────────────────────────────────────────────────────

def compile_binary(clang, src, polybench_c, utils, source_dir, out_bin,
                   extra_flags=None, dataset="LARGE_DATASET", timeout=180):
    defines = [f"-D{dataset}", "-DPOLYBENCH_TIME"]
    return compile_c(clang, [src, str(polybench_c)], [str(utils), str(source_dir)],
                     defines, out_bin, extra_flags=extra_flags, timeout=timeout)


# ── Clang driver flags (separate from -mllvm thresholds) ─────────────────────

# Clang driver flags that control optimization at a higher level than -mllvm.
# These act on the *driver* → *front-end* → *code-gen* pipeline, not just
# on individual pass thresholds.  We group them by effect so the LLM can
# reason about them alongside -mllvm flags.
CLANG_DRIVER_FLAG_GROUPS = [
    {
        "group":   "vectorize",
        "desc":    "Enable/tune SIMD vectorization",
        "flags": [
            "-fvectorize",                      # force loop-vectorize on
            "-fslp-vectorize",                  # force SLP on
            "-mprefer-vector-width=256",        # prefer 256-bit (AVX2) vectors
            "-mprefer-vector-width=512",        # prefer 512-bit (AVX-512) vectors
        ],
    },
    {
        "group":   "unroll",
        "desc":    "Control loop unrolling at the driver level",
        "flags": [
            "-funroll-loops",                   # enable aggressive unrolling
            "-funroll-all-loops",               # unroll even loops with unknown count
        ],
    },
    {
        "group":   "math",
        "desc":    "Relaxed floating-point for reassociation",
        "flags": [
            "-ffast-math",                      # enables reassoc+recip+fma globally
            "-fno-math-errno",                  # no errno after math; enables more opts
            "-ffp-contract=fast",               # fuse multiply-add across statements
        ],
    },
    {
        "group":   "inline",
        "desc":    "Inlining aggressiveness",
        "flags": [
            "-finline-functions",               # inline functions regardless of hint
            "-finline-hint-functions",          # inline only hint-marked functions
        ],
    },
    {
        "group":   "codegen",
        "desc":    "Code-generation quality",
        "flags": [
            "-fomit-frame-pointer",             # free one register (usually on by default at O3)
        ],
    },
]

# Clang pragma-level loop hints that can be inserted into the source.
# These produce !llvm.loop metadata — they are PER-LOOP, not global like -mllvm.
PRAGMA_LOOP_HINTS = [
    # Vectorization hints
    {
        "pragma":  "#pragma clang loop vectorize(enable)",
        "group":   "vectorize",
        "desc":    "Force loop vectorization even when cost model says no",
    },
    {
        "pragma":  "#pragma clang loop vectorize(enable) vectorize_width(8)",
        "group":   "vectorize",
        "desc":    "Force vectorization with 512-bit SIMD width (8×double)",
    },
    {
        "pragma":  "#pragma clang loop vectorize(enable) vectorize_width(4)",
        "group":   "vectorize",
        "desc":    "Force vectorization with 256-bit SIMD width (4×double)",
    },
    # Interleaving (software pipelining of multiple SIMD iterations)
    {
        "pragma":  "#pragma clang loop interleave(enable) interleave_count(2)",
        "group":   "vectorize",
        "desc":    "Interleave 2 vector iterations to hide FP latency",
    },
    {
        "pragma":  "#pragma clang loop interleave(enable) interleave_count(4)",
        "group":   "vectorize",
        "desc":    "Interleave 4 vector iterations",
    },
    # Unrolling hints
    {
        "pragma":  "#pragma clang loop unroll_count(4)",
        "group":   "unroll",
        "desc":    "Unroll the loop body 4 times",
    },
    {
        "pragma":  "#pragma clang loop unroll_count(8)",
        "group":   "unroll",
        "desc":    "Unroll the loop body 8 times",
    },
    {
        "pragma":  "#pragma clang loop unroll(full)",
        "group":   "unroll",
        "desc":    "Fully unroll a loop with a small/known bound",
    },
    # Distribute hint
    {
        "pragma":  "#pragma clang loop distribute(enable)",
        "group":   "distribute",
        "desc":    "Split loop with independent dependency chains into separate loops",
    },
]


def discover_clang_driver_options(clang: str, src: str, polybench_c: "Path",
                                   utils: "Path", source_dir: "Path",
                                   out_dir: "Path", runs: int = 3,
                                   pin_cpu=None) -> list:
    """
    Test each CLANG_DRIVER_FLAG_GROUPS flag independently against the -O3
    baseline and return results sorted by speedup.

    Returns list of {flag, group, desc, speedup, time_ms}.
    Flags that don't compile cleanly are skipped.
    """
    import tempfile as _tf, statistics as _st

    def _time(bin_path: str) -> float:
        cmd = (["taskset", "-c", str(pin_cpu)] if pin_cpu is not None else []) + [bin_path]
        try:
            subprocess.run(cmd, capture_output=True, timeout=60)
        except Exception:
            pass
        ts = []
        for _ in range(runs):
            try:
                r = subprocess.run(cmd, capture_output=True, text=True,
                                   timeout=60, errors="replace")
                if r.returncode == 0:
                    out = (r.stdout.strip() or r.stderr.strip())
                    ts.append(float(out.split()[-1]) * 1000.0)
            except Exception:
                pass
        return _st.median(ts) if ts else -1.0

    with _tf.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)

        base_bin = tmpdir / "base"
        ok, _ = compile_binary(clang, src, polybench_c, utils, source_dir, base_bin)
        if not ok:
            return []
        baseline = _time(str(base_bin))
        if baseline <= 0:
            return []

        results = []
        for grp in CLANG_DRIVER_FLAG_GROUPS:
            for flag in grp["flags"]:
                cand = tmpdir / f"drv_{flag.lstrip('-').replace('=','_')}"
                ok, _ = compile_binary(clang, src, polybench_c, utils, source_dir,
                                       cand, extra_flags=[flag])
                if not ok:
                    continue
                t = _time(str(cand))
                if t <= 0:
                    continue
                sp = baseline / t
                results.append({
                    "flag":     flag,
                    "group":    grp["group"],
                    "desc":     grp["desc"],
                    "speedup":  sp,
                    "time_ms":  t,
                })

    results.sort(key=lambda x: -x["speedup"])
    return results


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="O3 threshold parameter tuner")
    parser.add_argument("--program",   required=True)
    parser.add_argument("--config",    default="configs")
    parser.add_argument("--runs",      type=int, default=5)
    parser.add_argument("--pin-cpu",   type=int, default=None)
    parser.add_argument("--ir-lines",  type=int, default=150)
    parser.add_argument("--top-k",     type=int, default=5,
                        help="Top-K passes to show LLM (default=5)")
    args = parser.parse_args()

    loader  = ConfigLoader(config_dir=os.path.abspath(args.config))
    config  = loader.load_all()
    pin_cpu = args.pin_cpu if args.pin_cpu is not None else config.runtime.pin_cpu
    clang   = config.compiler.clang_path
    opt     = config.compiler.opt_path
    print(f"Compiler: {clang}  opt: {opt}  pin_cpu: {pin_cpu}")

    src = args.program
    if not os.path.exists(src):
        sys.exit(f"Not found: {src}")
    name        = Path(src).stem
    kernel_name = f"kernel_{name.replace('-', '_')}"
    utils       = find_polybench_utilities(src)
    if not utils:
        sys.exit("PolyBench utilities not found")
    source_dir  = Path(src).resolve().parent
    polybench_c = utils / "polybench.c"

    # ── Step 1: Find kernel line range ───────────────────────────────────────
    kline_start, kline_end = get_kernel_line_range(src, kernel_name)
    print(f"\n[Step 1] Kernel '{kernel_name}': lines {kline_start}–{kline_end}")

    # ── Step 2: Extract O3 passes for kernel (opt -debug-pass-manager on O3 IR) ──
    print("\n[Step 2] Extracting O3 passes for kernel...")
    # Compile with noinline patch so kernel stays visible after O3
    o3_ir = _gen_o3_ir(clang, src, utils, source_dir, kernel_name)
    if not o3_ir:
        sys.exit("clang -O3 -emit-llvm failed")
    kernel_passes = extract_o3_passes_for_kernel(opt, o3_ir, kernel_name)
    print(f"  {len(kernel_passes)} unique passes ran on {kernel_name}")
    kernel_ir_text = extract_kernel_ir(o3_ir, kernel_name, max_lines=args.ir_lines)
    try:
        os.unlink(o3_ir)
    except Exception:
        pass

    # ── Step 3: Collect rich YAML remarks (source line + VF/IC + code snippet) ──
    print("\n[Step 3] Collecting rich optimization remarks (clang -O3, YAML format)...")
    kernel_remarks = extract_rich_remarks_yaml(clang, src, utils, source_dir, kernel_name)
    if kernel_remarks:
        for pname, entries in sorted(kernel_remarks.items(),
                                     key=lambda kv: -len([e for e in kv[1]
                                                          if e["type"] == "missed"])):
            missed_n = len([e for e in entries if e["type"] == "missed"])
            passed_n = len([e for e in entries if e["type"] == "passed"])
            if missed_n > 0 or passed_n > 0:
                print(f"  {pname}: {missed_n} missed, {passed_n} applied")
                for e in entries[:2]:
                    if e["type"] == "missed":
                        loc = f"{e.get('file','?')}:{e.get('line',0)}"
                        vf_str = f" VF={e['vf']}" if e.get("vf") else ""
                        print(f"    {loc}{vf_str}: {e['msg'][:70]}")
    else:
        print("  (YAML remarks unavailable — falling back to text remarks)")
        all_remarks    = extract_remarks_by_pass(clang, src, utils, source_dir)
        kernel_remarks = filter_remarks_to_kernel(all_remarks, kline_start, kline_end)

    # ── Step 4: Discover options from opt --help-hidden ───────────────────────
    print("\n[Step 4] Discovering tunable options from opt --help-hidden...")
    discovered_opts = discover_options_from_help(opt, kernel_passes)
    for pname, opts in discovered_opts.items():
        print(f"  {pname}: {len(opts)} options found")

    # ── Step 5: Score passes ──────────────────────────────────────────────────
    print("\n[Step 5] Scoring passes (kernel-only missed count × category weight)...")
    scored = score_passes(kernel_passes, kernel_remarks, discovered_opts)

    if not scored:
        # Fallback: include passes with options even if no kernel remarks
        scored = []
        for pname, opts in discovered_opts.items():
            if pname in kernel_passes:
                cat_name, cat_weight = _pass_category(pname)
                scored.append((cat_weight, pname, cat_name, 0, []))
        scored.sort(reverse=True, key=lambda x: x[0])

    print(f"\n{'Rank':<5} {'Pass':<30} {'Cat':<12} {'KernelMiss':<12} {'Score':<8}")
    print("-" * 68)
    for i, (sc, pname, cat, mc, _) in enumerate(scored[:8], 1):
        has_opts = "YES" if pname in discovered_opts else "NO"
        print(f"{i:<5} {pname:<30} {cat:<12} {mc:<12} {sc:.1f}  opts={has_opts}")

    top_passes = scored[:args.top_k]

    # ── Step 6: IR stats before/after candidate flags ─────────────────────────
    # Use clang -O3 -S -emit-llvm
    print("\n[Step 6] Measuring IR stats before/after candidate flag values...")
    baseline_stats = get_ir_stats(clang, src, utils, source_dir, kernel_name)
    print(f"  Baseline: {baseline_stats}")

    ir_diff_info = []
    for _, pname, cat, mc, _ in top_passes:
        opts = discovered_opts.get(pname, [])
        for opt_entry in opts[:2]:  # check first 2 options per pass
            flag = opt_entry["flag"].lstrip("-")
            probe_val = _pick_probe_value(flag)
            if probe_val is None:
                continue
            stats_mod = get_ir_stats(clang, src, utils, source_dir, kernel_name,
                                     extra_mllvm=[f"-{flag}={probe_val}"])
            if baseline_stats and stats_mod:
                delta_vec   = stats_mod.get("vector_ops", 0) - baseline_stats.get("vector_ops", 0)
                delta_instr = stats_mod.get("total_instr", 0) - baseline_stats.get("total_instr", 0)
                if delta_vec != 0 or delta_instr != 0:
                    ir_diff_info.append(
                        f"  -{flag}={probe_val}: vector_ops {baseline_stats.get('vector_ops',0)}"
                        f" → {stats_mod.get('vector_ops',0)} (Δ{delta_vec:+d}),"
                        f" total_instr Δ{delta_instr:+d}"
                    )
                    print(f"  [{pname}] -{flag}={probe_val}: Δvec={delta_vec:+d} Δinstr={delta_instr:+d}")

    # ── Step 7: LLM selection ─────────────────────────────────────────────────
    cpu_info = get_cpu_info()

    missed_block = []
    for _, pname, _, mc, entries in top_passes:
        missed_only = [e for e in entries if e["type"] == "missed"]
        if missed_only:
            # Use rich format if entries have source_snippet (from YAML remarks)
            if entries and "source_snippet" in entries[0]:
                missed_block.append(format_rich_remark_for_prompt(pname, entries,
                                                                   max_entries=3))
            else:
                missed_block.append(f"\n**{pname}** (kernel misses: {mc})")
                for e in missed_only[:4]:
                    missed_block.append(f"  {e['file']}:{e['line']} → \"{e['msg'][:90]}\"")

    opts_block = []
    for _, pname, cat, mc, _ in top_passes:
        opts = discovered_opts.get(pname, [])
        if not opts:
            continue
        opts_block.append(f"\n**{pname}** [category={cat}, kernel_misses={mc}]")
        for o in opts:
            opts_block.append(f"  {o['flag']}=<{o['type']}>  — {o['desc']}")

    prompt = f"""You are an expert LLVM compiler optimization engineer.
Your job: pick ≤3 -mllvm threshold flags and candidate values to improve {name} performance.

## Hardware
{cpu_info}

## Kernel (O0 IR excerpt — before any optimization)
```llvm
{kernel_ir_text}
```

## Ranked tunable passes (scored by kernel-only missed remarks × category weight)
{chr(10).join(f'{i+1}. score={sc:.1f}  {pn} [{cat}, {mc} kernel misses]'
              for i, (sc, pn, cat, mc, _) in enumerate(top_passes))}

## Missed optimization remarks (kernel function only, with source location and vectorization info)
{"(none)" if not missed_block else chr(10).join(missed_block)}

## IR change evidence (baseline vs. probing one candidate value per flag)
Baseline stats: {baseline_stats}
{"(no IR changes observed)" if not ir_diff_info else chr(10).join(ir_diff_info)}

## Available tunable flags per pass (from opt --help-hidden, numeric threshold only)
{chr(10).join(opts_block) if opts_block else "(none discovered)"}

## Selection rules
1. ONLY choose flags from the "Available tunable flags" list above.
2. NEVER choose: force-vector-width, force-vector-interleave, unroll-count, or any
   "force-" / "disable-" flag — these bypass the cost model and cause regressions.
3. Prefer flags where IR change evidence shows vector_ops increase (Δvec > 0).
4. If a miss says "cost 0 >= 0", the threshold flag with a NEGATIVE value is the fix.
5. ≤3 flags total, ≤4 candidates per flag (total grid ≤ 64 combinations).
6. Include the default value as one candidate to confirm it's the baseline.

Output strict JSON (no markdown):
{{
  "analysis": "<2 sentences: key bottleneck and why these flags>",
  "parameters": [
    {{"flag": "-slp-threshold", "candidates": [-4, -2, -1, 0]}},
    {{"flag": "-licm-max-num-uses-traversed", "candidates": [8, 32, 128]}}
  ]
}}"""

    llm = LLMClient(config.llm)
    print(f"\n[Step 7] Querying LLM ({config.llm.model})...")
    resp = llm.call([
        {"role": "system",
         "content": "You are a compiler parameter tuning system. Output strict JSON only."},
        {"role": "user", "content": prompt},
    ])
    if not resp:
        sys.exit("LLM returned no response.")
    print(f"LLM response:\n{resp[:600]}")

    clean = resp.strip()
    for fence in ("```json", "```"):
        if clean.startswith(fence):
            clean = clean[len(fence):]
    if clean.endswith("```"):
        clean = clean[:-3]
    try:
        parsed = json.loads(clean.strip())
    except Exception as e:
        sys.exit(f"JSON parse error: {e}\nRaw: {resp[:300]}")

    params   = parsed.get("parameters", [])
    analysis = parsed.get("analysis", "")
    if not params:
        sys.exit("LLM returned no parameters.")

    print(f"\nAnalysis: {analysis}")
    print("Selected:")
    for p in params:
        print(f"  {p['flag']}: {p['candidates']}")

    # ── Step 8: Adaptive grid search ──────────────────────────────────────────
    # Phase A: test each flag independently
    # Phase B: combine best from each flag into joint search
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)

        # Baseline
        baseline_bin = tmpdir / "baseline"
        print("\n[Step 8] Compiling -O3 baseline...")
        ok, err = compile_binary(clang, src, polybench_c, utils, source_dir, baseline_bin)
        if not ok:
            sys.exit(f"Baseline compile failed:\n{err}")
        baseline_time = run_timing(str(baseline_bin), runs=args.runs, pin_cpu=pin_cpu)
        if baseline_time <= 0:
            sys.exit("Baseline timing failed.")
        print(f"Baseline -O3: {baseline_time:.2f} ms")

        # Phase A: independent flags
        print("\n  Phase A — independent flag search:")
        best_per_flag = {}   # flag_name → (best_time, best_val)
        all_results   = []

        for p_idx, p in enumerate(params):
            flag_name = p["flag"]
            candidates = p["candidates"]
            best_time_flag = baseline_time
            best_val_flag  = None
            for val in candidates:
                cand = tmpdir / f"phA_{p_idx}_{val}"
                mllvm_flags = ["-mllvm", f"{flag_name}={val}"]
                ok, _ = compile_binary(clang, src, polybench_c, utils, source_dir,
                                       cand, extra_flags=mllvm_flags)
                if not ok:
                    print(f"    {flag_name}={val} → compile FAILED")
                    continue
                t = run_timing(str(cand), runs=args.runs, pin_cpu=pin_cpu)
                if t <= 0:
                    continue
                sp  = baseline_time / t
                mark = " ← best" if t < best_time_flag else ""
                print(f"    {flag_name}={val} → {t:.1f} ms ({sp:.3f}x){mark}")
                all_results.append({"flags": f"{flag_name}={val}",
                                    "time_ms": t, "speedup": sp})
                if t < best_time_flag:
                    best_time_flag = t
                    best_val_flag  = val
            best_per_flag[flag_name] = (best_time_flag, best_val_flag)

        # Phase B: joint search with top-2 independently-best flags
        # Seed with the single best Phase A result (actually tested, not a synthetic combo)
        print("\n  Phase B — joint search (best pairs):")
        best_phA         = min(best_per_flag.items(), key=lambda kv: kv[1][0])
        best_joint_time  = best_phA[1][0]
        best_joint_flags = (f"-mllvm {best_phA[0]}={best_phA[1][1]}"
                            if best_phA[1][1] is not None else "")
        if len(params) >= 2:
            top_flags  = sorted(best_per_flag.items(),
                                key=lambda kv: kv[1][0])[:2]
            joint_vals = []
            for fn, (_, bv) in top_flags:
                # Search around best value (±1 step)
                orig_candidates = next(p["candidates"] for p in params
                                       if p["flag"] == fn)
                idx = orig_candidates.index(bv) if bv in orig_candidates else 0
                near = list({orig_candidates[max(0, idx-1)],
                              orig_candidates[idx],
                              orig_candidates[min(len(orig_candidates)-1, idx+1)]})
                joint_vals.append((fn, near))

            for combo in itertools.product(*[vs for _, vs in joint_vals]):
                flags = []
                desc  = []
                for (fn, _), val in zip(joint_vals, combo):
                    flags += ["-mllvm", f"{fn}={val}"]
                    desc.append(f"{fn}={val}")
                desc_str = ", ".join(desc)
                cand = tmpdir / f"phB_{'_'.join(str(v) for v in combo)}"
                ok, _ = compile_binary(clang, src, polybench_c, utils, source_dir,
                                       cand, extra_flags=flags)
                if not ok:
                    continue
                t = run_timing(str(cand), runs=args.runs, pin_cpu=pin_cpu)
                if t <= 0:
                    continue
                sp   = baseline_time / t
                mark = " ← BEST" if t < best_joint_time else ""
                print(f"    {desc_str} → {t:.1f} ms ({sp:.3f}x){mark}")
                all_results.append({"flags": desc_str, "time_ms": t, "speedup": sp})
                if t < best_joint_time:
                    best_joint_time  = t
                    best_joint_flags = " ".join(f"-mllvm {fn}={val}"
                                                 for (fn, _), val in zip(joint_vals, combo))

    # ── Report ────────────────────────────────────────────────────────────────
    best_speedup = baseline_time / best_joint_time if best_joint_time < baseline_time else 1.0
    print("\n" + "=" * 60)
    print(f"Program:      {name}")
    print(f"Baseline -O3: {baseline_time:.2f} ms")
    if best_speedup > 1.0:
        print(f"Best flags:   {best_joint_flags}")
        print(f"Best time:    {best_joint_time:.2f} ms")
        print(f"Speedup:      {best_speedup:.3f}x ({(best_speedup-1)*100:+.1f}%)")
    else:
        print("Result:       No improvement over -O3 baseline.")
        best_joint_flags = None
        best_joint_time  = baseline_time
    print("=" * 60)

    out_dir = Path("outputs")
    out_dir.mkdir(exist_ok=True)
    with open(out_dir / f"{name}_param_results.json", "w") as f:
        json.dump({
            "program":           name,
            "baseline_ms":       baseline_time,
            "analysis":          analysis,
            "kernel_line_range": [kline_start, kline_end],
            "scored_passes":     [(sc, pn, cat, mc) for sc, pn, cat, mc, _ in scored[:8]],
            "ir_baseline_stats": baseline_stats,
            "ir_diff_probes":    ir_diff_info,
            "parameters":        params,
            "results":           all_results,
            "best_flags":        best_joint_flags,
            "best_time_ms":      best_joint_time,
            "best_speedup":      best_speedup,
        }, f, indent=2)
    print(f"Results saved to outputs/{name}_param_results.json")


def _pick_probe_value(flag_name: str) -> "int | None":
    """Heuristic probe value for a flag name (for IR diff comparison)."""
    flag = flag_name.lower()
    # SLP vectorizer
    if "slp-threshold" in flag:
        return -10   # -4 often matches baseline; -10 shows clear IR difference
    if "slp" in flag and "reg-size" in flag:
        return 512
    if "slp" in flag and "look-ahead" in flag:
        return 8
    if "slp" in flag and "tree-size" in flag:
        return 2
    if "slp" in flag and "schedule" in flag:
        return 20
    if "slp" in flag and "recursion" in flag:
        return 12
    # Loop unroll
    if "unroll-threshold" in flag or "partial" in flag:
        return 300
    if "unroll-peel" in flag:
        return 8
    if "unroll-max-count" in flag or "upperbound" in flag:
        return 16
    # LICM
    if "licm" in flag and "traversed" in flag:
        return 32
    if "licm" in flag and "cap" in flag:
        return 50
    if "licm" in flag and "promotion" in flag:
        return 200
    if "licm" in flag and "n2" in flag:
        return 2000
    if "licm" in flag and "depth" in flag:
        return 3
    # Loop unswitch
    if "unswitch-threshold" in flag or "loop-unswitch" in flag:
        return 200
    if "unswitch-siblings" in flag:
        return 2
    if "unswitch-num-initial" in flag:
        return 5
    # Loop load elimination / loop distribute
    if "loop-load-elim" in flag or "runtime-check-per-loop" in flag:
        return 2
    if "loop-distribute" in flag:
        return 32
    # GVN
    if "gvn" in flag and "deps" in flag:
        return 300
    if "gvn" in flag and "recurse" in flag:
        return 1000
    if "gvn" in flag and "hoist" in flag:
        return 10
    # Inliner
    if "inline-threshold" in flag or "inlinecold" in flag or "inlinedefault" in flag:
        return 500
    if "inline-deferral" in flag:
        return 4
    # Instcombine
    if "instcombine-max-iter" in flag:
        return 3
    if "instcombine-max-num-phis" in flag:
        return 64
    if "instcombine-negator" in flag:
        return 8
    # Jump threading
    if "jump-threading-threshold" in flag:
        return 12
    if "jump-threading-implication" in flag:
        return 2
    # SCEV / vectorize
    if "scev" in flag and "threshold" in flag:
        return 64
    if "trip" in flag:
        return 16
    if "vectorize-num-stores" in flag:
        return 32
    # EarlyCSE
    if "earlycse" in flag:
        return 50
    # CallSiteSplitting
    if "callsite-splitting" in flag:
        return 3
    # SpeculativeExecution
    if "spec-exec" in flag and "not-hoisted" in flag:
        return 5
    if "spec-exec" in flag and "cost" in flag:
        return 10
    # SimplifyCFG
    if "simplifycfg" in flag:
        return 5
    # DSE MemorySSA
    if "dse-memoryssa" in flag:
        return 2000
    return None


if __name__ == "__main__":
    main()
