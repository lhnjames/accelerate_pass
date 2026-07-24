"""Reproducible statistics for paired compiler-performance measurements."""
from __future__ import annotations

import math
import random
import statistics
from dataclasses import asdict, dataclass
from typing import Iterable, Sequence


@dataclass(frozen=True)
class PairedPerformanceSummary:
    baseline_seconds: tuple[float, ...]
    candidate_seconds: tuple[float, ...]
    paired_speedups: tuple[float, ...]
    sample_count: int
    median_baseline_seconds: float
    median_candidate_seconds: float
    median_paired_speedup: float
    speedup_iqr: float
    speedup_cv: float
    bootstrap_ci95: tuple[float, float]
    bootstrap_seed: int
    bootstrap_resamples: int

    def to_dict(self) -> dict:
        return asdict(self)


def _validated_times(values: Iterable[float], label: str) -> tuple[float, ...]:
    result = tuple(float(value) for value in values)
    if not result:
        raise ValueError(f"{label} samples are empty")
    if any(not math.isfinite(value) or value <= 0 for value in result):
        raise ValueError(f"{label} samples must be finite and positive")
    return result


def _linear_percentile(values: Sequence[float], probability: float) -> float:
    """NumPy-compatible linear percentile without adding a dependency here."""
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = probability * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def bootstrap_median_ci(
    values: Sequence[float],
    *,
    confidence: float = 0.95,
    resamples: int = 10_000,
    seed: int = 0,
) -> tuple[float, float]:
    """Return a deterministic percentile bootstrap interval for the median."""
    if not values:
        raise ValueError("bootstrap values are empty")
    if not 0.0 < confidence < 1.0:
        raise ValueError("confidence must be between zero and one")
    if resamples <= 0:
        raise ValueError("resamples must be positive")
    numeric = tuple(float(value) for value in values)
    if any(not math.isfinite(value) for value in numeric):
        raise ValueError("bootstrap values must be finite")

    rng = random.Random(seed)
    size = len(numeric)
    medians = [
        statistics.median(numeric[rng.randrange(size)] for _ in range(size))
        for _ in range(resamples)
    ]
    tail = (1.0 - confidence) / 2.0
    return (_linear_percentile(medians, tail),
            _linear_percentile(medians, 1.0 - tail))


def summarize_paired_performance(
    baseline_seconds: Iterable[float],
    candidate_seconds: Iterable[float],
    *,
    bootstrap_seed: int = 0,
    bootstrap_resamples: int = 10_000,
) -> PairedPerformanceSummary:
    """Summarize alternating O3/candidate samples as paired speedups.

    Pairing is positional: sample ``i`` from O3 is divided by sample ``i`` from
    the candidate.  Callers must therefore preserve the alternating collection
    order and must not independently sort the two sample vectors.
    """
    baseline = _validated_times(baseline_seconds, "baseline")
    candidate = _validated_times(candidate_seconds, "candidate")
    if len(baseline) != len(candidate):
        raise ValueError("baseline and candidate sample counts differ")

    speedups = tuple(base / tuned for base, tuned in zip(baseline, candidate))
    q1 = _linear_percentile(speedups, 0.25)
    q3 = _linear_percentile(speedups, 0.75)
    mean = statistics.fmean(speedups)
    cv = statistics.stdev(speedups) / mean if len(speedups) > 1 else 0.0
    ci = bootstrap_median_ci(
        speedups, resamples=bootstrap_resamples, seed=bootstrap_seed)
    return PairedPerformanceSummary(
        baseline_seconds=baseline,
        candidate_seconds=candidate,
        paired_speedups=speedups,
        sample_count=len(speedups),
        median_baseline_seconds=statistics.median(baseline),
        median_candidate_seconds=statistics.median(candidate),
        median_paired_speedup=statistics.median(speedups),
        speedup_iqr=q3 - q1,
        speedup_cv=cv,
        bootstrap_ci95=ci,
        bootstrap_seed=bootstrap_seed,
        bootstrap_resamples=bootstrap_resamples,
    )
