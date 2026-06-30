"""COMET — Compiler Observable-guided Metric-Efficient Tuner."""
from pathlib import Path
import tempfile

# Route all temp files into the project's own tmp/ directory.
_project_tmp = Path(__file__).resolve().parent.parent / "tmp"
_project_tmp.mkdir(parents=True, exist_ok=True)
tempfile.tempdir = str(_project_tmp)

from src.config import ConfigLoader, COMETConfig
from src.compiler_manager import CompilerRunner
from src.static_analyzer import RemarksExtractor
from src.llm_client import LLMClient
from src.data_structures import Remark, RemarksData

__all__ = [
    'ConfigLoader', 'COMETConfig',
    'CompilerRunner',
    'RemarksExtractor',
    'LLMClient',
    'Remark', 'RemarksData',
]
