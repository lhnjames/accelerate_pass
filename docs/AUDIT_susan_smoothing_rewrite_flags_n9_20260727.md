# Audit: `automotive_susan_smoothing` rewrite+flags n=9

Date: 2026-07-27

## Conclusion

The original case-study result (`confirmed_speedup=1.2559244022694946`,
IQR `[1.2371099966245969, 1.2656799188822923]`, `n_positive=9/9`) preserved
only aggregate statistics. The nine original `(baseline_ms, candidate_ms,
ratio)` tuples were not returned or logged by `confirm_result_external()`,
so they cannot be reconstructed.

To obtain auditable per-round evidence, the surviving original binaries were
remeasured on `dss-dgx-a`, pinned to CPU 8:

- baseline: `/tmp/ss2_base`
- rewrite+flags: `/tmp/ss2_both`
- candidate flag: `-flto`

The existing params-only queue process was pinned to CPU 2; CPU 8 was idle at
the time of this audit.

## Per-round replication

Each row is one alternating pair. Each individual `baseline_ms` or
`candidate_ms` value is itself the trimmed mean of four process executions:
sort the four wall-clock times, discard the minimum and maximum, and average
the middle two.

| Round | baseline_ms | candidate_ms | baseline/candidate |
|---:|---:|---:|---:|
| 1 | 28.440151480027 | 22.991353413090 | 1.236993358722 |
| 2 | 28.268623631448 | 22.975377622060 | 1.230387769745 |
| 3 | 28.598351986147 | 22.400993038900 | 1.276655545425 |
| 4 | 28.665264020674 | 22.790153510869 | 1.257791616323 |
| 5 | 28.621311648749 | 22.907433449291 | 1.249433364593 |
| 6 | 28.265542932786 | 22.976569482125 | 1.230189866019 |
| 7 | 28.190862969495 | 23.022377979942 | 1.224498311775 |
| 8 | 28.383759432472 | 22.995721548796 | 1.234306102213 |
| 9 | 28.576584067196 | 23.173233377747 | 1.233172065433 |

Sorted ratios:

```text
1.224498311775
1.230189866019
1.230387769745
1.233172065433
1.234306102213
1.236993358722
1.249433364593
1.257791616323
1.276655545425
```

Replication summary:

- median paired speedup: `1.234306102213x`
- IQR under the project's index-based definition: `[1.230387769745, 1.249433364593]`
- positive pairs: `9/9`
- baseline median: `28.440151480027 ms`
- candidate median: `22.976569482125 ms`
- baseline CV: `0.624063488112%`
- candidate CV: `0.948407014848%`

The replication supports a stable rewrite+flags gain of roughly 23--26%, but
does not reproduce the exact original point estimate of `1.2559x`. The two
IQRs overlap.

## Exact aggregation rule

For nine valid pairs:

```python
ratio[i] = baseline_ms[i] / candidate_ms[i]
confirmed_speedup = median(ratio)
q1 = sorted(ratio)[9 // 4]          # index 2
q3 = sorted(ratio)[(3 * 9) // 4]    # index 6
n_positive = count(ratio > 1.0)
```

This is the implementation in `optimize.py::confirm_result_external`.

## Other strict n=9 three-leg candidates

No other audited candidate has all three legs stable:

| Kernel | rewrite-only | flags-only | rewrite+flags | Why rejected |
|---|---:|---:|---:|---|
| `2mm` | 8.8828x, 9/9 | 1.0067x, 5/9, IQR `[0.9622, 1.0734]` | 9.5750x, 9/9 | flags-only fails |
| `syrk` | 3.0536x, 9/9 | 0.9294x, 4/9, IQR `[0.8651, 1.0674]` | 3.1507x, 9/9 | flags-only fails |
| `durbin`, paired rewrite | 0.951x, 1/9 | 1.626x, 9/9 | 1.495x | rewrite-only fails |
| `durbin`, branch-free rewrite | 0.986x, 4/9 | 1.626x (reused) | 1.328x | rewrite-only fails |
| `seidel-2d` sliding-column rewrite | correctness failure | not strictly n=9 | not measured | invalid rewrite |

The actual stored `2mm` and `syrk` task outputs do contain the combined
measurements; earlier summaries saying those combined legs were not measured
were inaccurate. This does not change their rejection because flags-only
fails in both cases.
