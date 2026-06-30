import os
import yaml
from dataclasses import dataclass
from typing import Dict, Any, Optional
from pathlib import Path
import logging

logger = logging.getLogger(__name__)


@dataclass
class LLMConfig:
    api_key: str
    model: str
    base_url: str
    timeout_seconds: int
    max_tokens: int
    temperature: float
    call_budget: int
    reasoning_effort: Optional[str] = None
    thinking_enabled: bool = False


@dataclass
class CompilerConfig:
    clang_path: str
    clang_cxx_path: str
    opt_path: str
    llc_path: str
    timeout_seconds: int


@dataclass
class MCTSConfig:
    iterations: int
    max_depth: int
    min_sequence_length: int
    max_sequence_length: int
    puct_exploration_constant: float
    q_value_scale: float
    expansion_visit_threshold: int
    measure_visit_threshold: int
    periodic_measure_interval: int
    random_seed: Optional[int] = None
    enable_diversity_fallback: bool = True


@dataclass
class RuntimeConfig:
    total_budget: int
    baseline_measurement_cost: int
    seed_measurement_cost: int
    mcts_search_budget: int
    top_candidates_budget: int
    local_refinement_budget: int
    measurement_timeout: int
    measurement_repeat: int
    pin_cpu: Optional[int] = None          # taskset -c <N>; None = no pinning
    flush_cache_before_run: bool = False   # software LLC flush before each run
    confirm_speedup_threshold: float = 5.0 # re-measure if speedup > N% to confirm
    drift_cap: float = 0.20                # if |drift-1| > cap, compare vs o3_now directly
    # Two-level measurement: fast during MCTS tree search, accurate in post-search.
    # mcts_measurement_runs=1 makes each MCTS measurement 9× faster than measurement_repeat=9,
    # allowing far more iterations within the same wall time.
    # Post-search uses measurement_repeat for the final top-K timing.
    mcts_measurement_runs: int = 1         # runs per measurement during MCTS search
    measurement_cache_path: str = "outputs/measurement_cache.json"  # persistent cache
    enable_discovered_pass_parameters: bool = True
    pass_parameter_cache_path: Optional[str] = None
    max_micro_configs: int = 8
    max_micro_tunable_families: int = 2
    micro_search_min_anchor_speedup_pct: float = 0.5
    enable_polly_passes: bool = False
    stagnation_llm_budget: int = 4
    stagnation_llm_cooldown: int = 10
    catastrophic_slowdown_threshold: float = 10.0
    backoff_slowdown_tolerance_pct: float = 3.0
    seed_backoff_patience: int = 4
    seed_backoff_penalty: float = 2.0
    catastrophic_backoff_penalty: float = 8.0
    llm_backtrack_penalty_step: float = 1.0
    llm_backtrack_penalty_max: float = 4.0
    positive_reward_scale: float = 100.0


@dataclass
class OptimizationConfig:
    max_iterations: int
    min_no_improvement_rounds: int
    max_candidates: int
    max_sequence_length: int
    early_stage_llm_frequency: int
    late_stage_llm_frequency: int
    late_stage_threshold: int
    program_path: Optional[str]
    config_dir: str
    output_dir: str


@dataclass
class ProfilingConfig:
    perf_enabled: bool = True
    vtune_enabled: bool = False   # VTune config exists but is not yet implemented
    trace_functions: int = 5
    cpu_time_threshold: float = 0.85


@dataclass
class COMETConfig:
    compiler: CompilerConfig
    llm: LLMConfig
    mcts: MCTSConfig
    runtime: RuntimeConfig
    optimization: OptimizationConfig
    profiling: ProfilingConfig
    output_dir: str
    verbose: bool
    log_level: str
    config_dir: str = "./configs"
    knowledge_base_file: Optional[str] = None
    bottleneck_mapping: Optional[Dict[str, Any]] = None


class ConfigLoader:
    def __init__(self, config_dir: str = None):
        if config_dir is None:
            config_dir = os.path.dirname(os.path.abspath(__file__))
            config_dir = os.path.join(os.path.dirname(config_dir), "configs")

        self.config_dir = config_dir
        self.yaml_config: Dict[str, Any] = {}
        self.env_config: Dict[str, Any] = {}
        self.pass_database: Dict[str, Any] = {}
        self.bottleneck_mapping: Dict[str, Any] = {}

    def load_yaml_config(self, config_file: str = "config.yaml") -> Dict[str, Any]:
        config_path = os.path.join(self.config_dir, config_file)
        if not os.path.exists(config_path):
            raise FileNotFoundError(f"Config file not found: {config_path}")

        with open(config_path, 'r') as f:
            self.yaml_config = yaml.safe_load(f)

        logger.info(f"Loaded YAML config from {config_path}")
        return self.yaml_config

    def load_env_config(self, env_file: str = ".env") -> Dict[str, Any]:
        env_path = os.path.join(os.path.dirname(self.config_dir), env_file)
        if os.path.exists(env_path):
            with open(env_path, 'r') as f:
                for line in f:
                    line = line.strip()
                    if line and not line.startswith("#"):
                        if '=' in line:
                            key, value = line.split('=', 1)
                            self.env_config[key.strip()] = value.strip()
                        else:
                            logger.warning(f"Invalid .env line (missing '='): {line}")
            logger.info(f"Loaded ENV config from {env_path}")
        else:
            logger.warning(f"ENV file not found: {env_path}, using system environment variables")
            # Load from system environment
            for key in ['DEEPSEEK_API_KEY', 'DEEPSEEK_MODEL', 'CLANG_PATH', 'OPT_PATH']:
                if key in os.environ:
                    self.env_config[key] = os.environ[key]

        return self.env_config

    def load_pass_database(self) -> Dict[str, Any]:
        pass_db_path = os.path.join(self.config_dir, "pass_database.yaml")
        if not os.path.exists(pass_db_path):
            raise FileNotFoundError(f"Pass database not found: {pass_db_path}")

        with open(pass_db_path, 'r') as f:
            self.pass_database = yaml.safe_load(f)

        logger.info(f"Loaded pass database from {pass_db_path}")
        return self.pass_database

    def load_bottleneck_mapping(self) -> Dict[str, Any]:
        bottleneck_path = os.path.join(self.config_dir, "bottleneck_mapping.yaml")
        if not os.path.exists(bottleneck_path):
            raise FileNotFoundError(f"Bottleneck mapping not found: {bottleneck_path}")

        with open(bottleneck_path, 'r') as f:
            self.bottleneck_mapping = yaml.safe_load(f)

        logger.info(f"Loaded bottleneck mapping from {bottleneck_path}")
        return self.bottleneck_mapping

    def load_all(self) -> COMETConfig:
        self.load_yaml_config()
        self.load_env_config()
        self.load_pass_database()
        self.load_bottleneck_mapping()

        return self.build_comet_config()

    def build_comet_config(self) -> COMETConfig:
        # Get values from YAML or ENV with fallbacks
        # Use a sentinel to distinguish between "not found" and "None value"
        _NOTFOUND = object()

        def _parse_bool(val) -> bool:
            if isinstance(val, bool):
                return val
            if isinstance(val, str):
                return val.strip().lower() not in ('false', '0', 'no', 'off', '')
            return bool(val)

        def get_value(path: str, default: Any = _NOTFOUND) -> Any:
            yaml_val = self._get_nested(self.yaml_config, path)
            env_key = path.replace('.', '_').upper()
            env_val = self.env_config.get(env_key)

            if yaml_val is not None:
                return yaml_val
            if env_val is not None:
                return env_val
            if default is not _NOTFOUND:
                return default
            raise ValueError(f"Configuration not found for {path}")

        compiler = CompilerConfig(
            clang_path=get_value("compiler.clang_path", "/usr/bin/clang"),
            clang_cxx_path=get_value("compiler.clang_cxx_path", "/usr/bin/clang++"),
            opt_path=get_value("compiler.opt_path", "/usr/bin/opt"),
            llc_path=get_value("compiler.llc_path", "/usr/bin/llc"),
            timeout_seconds=get_value("compiler.timeout_seconds", 300),
        )

        llm = LLMConfig(
            api_key=get_value("llm.api_key", self.env_config.get("DEEPSEEK_API_KEY")),
            model=get_value("llm.model", self.env_config.get("DEEPSEEK_MODEL", "deepseek-reasoner")),
            base_url=get_value("llm.base_url", self.env_config.get("DEEPSEEK_BASE_URL", "https://api.deepseek.com")),
            timeout_seconds=int(get_value("llm.timeout_seconds", 60)),
            max_tokens=int(get_value("llm.max_tokens", 3500)),
            temperature=float(get_value("llm.temperature", 0.5)),
            call_budget=int(get_value("llm.call_budget", 20)),
            reasoning_effort=get_value("llm.reasoning_effort", None),
            thinking_enabled=_parse_bool(get_value("llm.thinking_enabled", False)),
        )

        mcts = MCTSConfig(
            iterations=get_value("mcts.iterations", 300),
            max_depth=get_value("mcts.max_depth", 30),
            min_sequence_length=get_value("mcts.min_sequence_length", 4),
            max_sequence_length=get_value("mcts.max_sequence_length", 30),
            puct_exploration_constant=get_value("mcts.puct_exploration_constant", 1.5),
            q_value_scale=float(get_value("mcts.q_value_scale", 10.0)),
            expansion_visit_threshold=get_value("mcts.expansion_visit_threshold", 3),
            measure_visit_threshold=get_value("mcts.measure_visit_threshold", 5),
            periodic_measure_interval=get_value("mcts.periodic_measure_interval", 20),
            random_seed=get_value("mcts.random_seed", None),
            enable_diversity_fallback=_parse_bool(
                get_value("mcts.enable_diversity_fallback", True)),
        )

        runtime = RuntimeConfig(
            total_budget=get_value("runtime.total_budget", 100),
            baseline_measurement_cost=get_value("runtime.baseline_measurement_cost", 1),
            seed_measurement_cost=get_value("runtime.seed_measurement_cost", 1),
            mcts_search_budget=get_value("runtime.mcts_search_budget", 58),
            top_candidates_budget=get_value("runtime.top_candidates_budget", 20),
            local_refinement_budget=get_value("runtime.local_refinement_budget", 20),
            measurement_timeout=get_value("runtime.measurement_timeout", 600),
            measurement_repeat=get_value("runtime.measurement_repeat", 1),
            pin_cpu=get_value("runtime.pin_cpu", None),
            flush_cache_before_run=_parse_bool(get_value("runtime.flush_cache_before_run", False)),
            confirm_speedup_threshold=float(get_value("runtime.confirm_speedup_threshold", 5.0)),
            drift_cap=float(get_value("runtime.drift_cap", 0.20)),
            mcts_measurement_runs=int(get_value("runtime.mcts_measurement_runs", 1)),
            measurement_cache_path=get_value("runtime.measurement_cache_path",
                                             "outputs/measurement_cache.json"),
            enable_discovered_pass_parameters=_parse_bool(
                get_value("runtime.enable_discovered_pass_parameters", True)),
            pass_parameter_cache_path=get_value("runtime.pass_parameter_cache_path", None),
            max_micro_configs=int(get_value("runtime.max_micro_configs", 8)),
            max_micro_tunable_families=int(get_value("runtime.max_micro_tunable_families", 2)),
            micro_search_min_anchor_speedup_pct=float(
                get_value("runtime.micro_search_min_anchor_speedup_pct", 0.5)),
            enable_polly_passes=_parse_bool(get_value("runtime.enable_polly_passes", False)),
            stagnation_llm_budget=int(get_value("runtime.stagnation_llm_budget", 4)),
            stagnation_llm_cooldown=int(get_value("runtime.stagnation_llm_cooldown", 10)),
            catastrophic_slowdown_threshold=float(
                get_value("runtime.catastrophic_slowdown_threshold", 10.0)),
            backoff_slowdown_tolerance_pct=float(
                get_value("runtime.backoff_slowdown_tolerance_pct", 3.0)),
            seed_backoff_patience=int(get_value("runtime.seed_backoff_patience", 4)),
            seed_backoff_penalty=float(get_value("runtime.seed_backoff_penalty", 2.0)),
            catastrophic_backoff_penalty=float(
                get_value("runtime.catastrophic_backoff_penalty", 8.0)),
            llm_backtrack_penalty_step=float(
                get_value("runtime.llm_backtrack_penalty_step", 1.0)),
            llm_backtrack_penalty_max=float(
                get_value("runtime.llm_backtrack_penalty_max", 4.0)),
            positive_reward_scale=float(get_value("runtime.positive_reward_scale", 100.0)),
        )

        optimization = OptimizationConfig(
            max_iterations=get_value("optimization.max_iterations", 150),
            min_no_improvement_rounds=get_value("optimization.min_no_improvement_rounds", 20),
            max_candidates=get_value("optimization.max_candidates", 10),
            max_sequence_length=get_value("optimization.max_sequence_length", 15),
            early_stage_llm_frequency=get_value("optimization.early_stage_llm_frequency", 1),
            late_stage_llm_frequency=get_value("optimization.late_stage_llm_frequency", 2),
            late_stage_threshold=get_value("optimization.late_stage_threshold", 50),
            program_path=get_value("optimization.program_path", None),
            config_dir=get_value("optimization.config_dir", "./configs"),
            output_dir=get_value("optimization.output_dir", "./results"),
        )

        profiling = ProfilingConfig(
            perf_enabled=_parse_bool(get_value("profiling.perf_enabled", True)),
            vtune_enabled=_parse_bool(get_value("profiling.vtune_enabled", False)),
            trace_functions=int(get_value("profiling.trace_functions", 5)),
            cpu_time_threshold=float(get_value("profiling.cpu_time_threshold", 0.85)),
        )

        _kb_file_cfg = get_value("knowledge_base.file_path", None)

        return COMETConfig(
            compiler=compiler,
            llm=llm,
            mcts=mcts,
            runtime=runtime,
            optimization=optimization,
            profiling=profiling,
            output_dir=get_value("output.directory", "./outputs"),
            verbose=_parse_bool(get_value("output.verbose", False)),
            log_level=get_value("output.log_level", "INFO"),
            config_dir=self.config_dir,
            knowledge_base_file=_kb_file_cfg,
            bottleneck_mapping=self.bottleneck_mapping,
        )

    @staticmethod
    def _get_nested(d: Dict, path: str) -> Any:
        keys = path.split('.')
        value = d
        for key in keys:
            if isinstance(value, dict):
                value = value.get(key)
            else:
                return None
            if value is None:
                return None
        return value
