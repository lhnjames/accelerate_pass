# Pass and runtime analysis

Audit the complete LLVM O3 pass pipeline for the supplied kernel before any
parameter tuning. For every pass that ran, explain its role, whether it fired,
was a no-op, or was skipped, and connect that observation to the kernel's
runtime control/data-flow, hardware counters, remarks, and measured history.
Identify concrete mismatches between the pass's intended effect and what the
program actually needs. Only recommend numeric LLVM 21 cost-model parameters
that appear in the supplied `opt-21 --help-hidden` inventory; never invent
flags or use force/disable/testing-only controls. Return strict JSON only.
