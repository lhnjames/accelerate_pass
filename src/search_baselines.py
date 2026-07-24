"""Equal-budget, non-LLM baselines over a shared discrete candidate catalog."""
from __future__ import annotations

import math
import random
import time
import warnings
from dataclasses import asdict, dataclass
from itertools import product
from typing import Callable, List, Optional, Sequence, Tuple, Union

import numpy as np
from scipy.stats import norm
from sklearn.gaussian_process import GaussianProcessRegressor
from sklearn.gaussian_process.kernels import ConstantKernel, Matern, WhiteKernel
from sklearn.exceptions import ConvergenceWarning


@dataclass(frozen=True)
class Candidate:
    values: Tuple[float, ...]
    flags: Tuple[str, ...]
    label: str = ""


@dataclass(frozen=True)
class ParameterAxis:
    flag: str
    values: Tuple[float, ...]


def build_candidate_catalog(axes: Sequence[ParameterAxis], *,
                            max_candidates: int = 100_000) -> list[Candidate]:
    """Create one shared deterministic Cartesian catalog for all methods."""
    if not axes:
        raise ValueError("at least one parameter axis is required")
    normalized = []
    for axis in axes:
        flag = axis.flag if axis.flag.startswith("--") else f"--{axis.flag.lstrip('-')}"
        name = flag[2:].lower()
        if any(token in name.split("-") for token in ("force", "disable")):
            raise ValueError(f"unsafe cost-model override axis: {flag}")
        values = tuple(float(value) for value in axis.values)
        if not values or any(not math.isfinite(value) for value in values):
            raise ValueError(f"axis {flag} has no finite values")
        if len(set(values)) != len(values):
            raise ValueError(f"axis {flag} contains duplicate values")
        normalized.append((flag, values))
    size = math.prod(len(values) for _, values in normalized)
    if size > max_candidates:
        raise ValueError(f"candidate catalog has {size} points; limit is {max_candidates}")
    catalog = []
    for combination in product(*(values for _, values in normalized)):
        flags = tuple(
            item for (flag, _), value in zip(normalized, combination)
            for item in ("-mllvm", f"{flag}={value:g}")
        )
        label = ",".join(f"{flag}={value:g}"
                          for (flag, _), value in zip(normalized, combination))
        catalog.append(Candidate(tuple(combination), flags, label))
    return catalog


@dataclass(frozen=True)
class Evaluation:
    """One equal-budget evaluator outcome; failures still consume a trial."""
    objective: float
    success: bool = True
    error: str = ""


@dataclass(frozen=True)
class Trial:
    index: int
    candidate: Candidate
    objective: float
    success: bool = True
    error: str = ""
    elapsed_seconds: float = 0.0


@dataclass(frozen=True)
class SearchResult:
    method: str
    seed: int
    budget: int
    trials: Tuple[Trial, ...]

    @property
    def best(self) -> Trial:
        successful = [trial for trial in self.trials if trial.success]
        if not successful:
            raise ValueError("search has no successful trials")
        return max(successful, key=lambda trial: trial.objective)

    @property
    def failed_count(self) -> int:
        return sum(not trial.success for trial in self.trials)

    def to_dict(self) -> dict:
        return {
            "method": self.method,
            "seed": self.seed,
            "budget": self.budget,
            "failed_count": self.failed_count,
            "trials": [asdict(trial) for trial in self.trials],
        }


Evaluator = Callable[[Candidate], Union[float, Evaluation]]

_FAILED_OBJECTIVE = -1.0e12


def _measure(index: int, candidate: Candidate, evaluate: Evaluator) -> Trial:
    started = time.monotonic()
    try:
        raw = evaluate(candidate)
        result = raw if isinstance(raw, Evaluation) else Evaluation(float(raw))
    except Exception as exc:  # failed compile/run counts against the budget
        result = Evaluation(_FAILED_OBJECTIVE, False,
                            f"{type(exc).__name__}: {exc}")
    elapsed = time.monotonic() - started
    objective = float(result.objective)
    if not math.isfinite(objective):
        objective = _FAILED_OBJECTIVE
        result = Evaluation(objective, False,
                            result.error or "non-finite objective")
    if not result.success:
        objective = _FAILED_OBJECTIVE
    return Trial(index, candidate, objective, result.success,
                 result.error, elapsed)


def _validate(candidates: Sequence[Candidate], budget: int) -> int:
    if not candidates:
        raise ValueError("candidate catalog is empty")
    if budget <= 0:
        raise ValueError("budget must be positive")
    width = len(candidates[0].values)
    if any(len(candidate.values) != width for candidate in candidates):
        raise ValueError("all candidates must have the same numeric width")
    return min(budget, len(candidates))


def random_search(candidates: Sequence[Candidate], budget: int, seed: int,
                  evaluate: Evaluator) -> SearchResult:
    actual_budget = _validate(candidates, budget)
    rng = random.Random(seed)
    indices = rng.sample(range(len(candidates)), actual_budget)
    trials = tuple(_measure(i, candidates[i], evaluate) for i in indices)
    return SearchResult("random", seed, actual_budget, trials)


def bayesian_search(candidates: Sequence[Candidate], budget: int, seed: int,
                    evaluate: Evaluator, initial_points: int = 5) -> SearchResult:
    """Gaussian-process expected-improvement search without replacement."""
    actual_budget = _validate(candidates, budget)
    rng = random.Random(seed)
    all_indices = list(range(len(candidates)))
    initial_n = min(max(1, initial_points), actual_budget)
    selected = rng.sample(all_indices, initial_n)
    trials: List[Trial] = []

    def measure(index: int) -> None:
        candidate = candidates[index]
        trials.append(_measure(index, candidate, evaluate))

    for index in selected:
        measure(index)

    x_all = np.asarray([candidate.values for candidate in candidates], dtype=float)
    # Normalize dimensions once from the complete, shared catalog. Constant
    # dimensions remain zero and do not destabilize the GP.
    means = x_all.mean(axis=0)
    scales = x_all.std(axis=0)
    scales[scales == 0] = 1.0
    x_all = (x_all - means) / scales

    while len(trials) < actual_budget:
        measured = {trial.index for trial in trials}
        remaining = [i for i in all_indices if i not in measured]
        x_train = x_all[[trial.index for trial in trials]]
        y_train = np.asarray([trial.objective for trial in trials])
        kernel = (ConstantKernel(1.0, (1e-3, 1e3))
                  * Matern(length_scale=np.ones(x_all.shape[1]), nu=2.5)
                  + WhiteKernel(noise_level=1e-5, noise_level_bounds=(1e-9, 1e-1)))
        model = GaussianProcessRegressor(
            kernel=kernel, normalize_y=True, random_state=seed,
            n_restarts_optimizer=0)
        # Boundary convergence is common for a small discrete catalog and is
        # not an experimental failure; the fitted model remains usable.
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ConvergenceWarning)
            model.fit(x_train, y_train)
        mu, sigma = model.predict(x_all[remaining], return_std=True)
        incumbent = float(np.max(y_train))
        improvement = mu - incumbent
        with np.errstate(divide="ignore", invalid="ignore"):
            z = np.divide(improvement, sigma, out=np.zeros_like(improvement),
                          where=sigma > 1e-12)
            expected_improvement = improvement * norm.cdf(z) + sigma * norm.pdf(z)
        # Stable deterministic tie break by catalog index.
        best_pos = max(range(len(remaining)),
                       key=lambda pos: (expected_improvement[pos], -remaining[pos]))
        measure(remaining[best_pos])

    return SearchResult("bayesian_gp_ei", seed, actual_budget, tuple(trials))
