import os
import subprocess
import tempfile
import logging
from typing import Optional, Tuple
from pathlib import Path

from src.config import COMETConfig
from src.polybench_paths import find_polybench_utilities, POLYBENCH_DIR_NAMES

logger = logging.getLogger(__name__)


class CompilerRunner:
    def __init__(self, config: COMETConfig, pass_database: dict = None):
        self.config = config.compiler if hasattr(config, 'compiler') else config

    _POLYBENCH_DIR_NAMES = POLYBENCH_DIR_NAMES

    def _find_polybench_utilities(self, input_file: str) -> Optional[Path]:
        return find_polybench_utilities(input_file)

    def extract_ir(self, input_file: str) -> Tuple[bool, Optional[str], Optional[str]]:
        """
        Compile source to O0 IR with -disable-O0-optnone so opt can apply
        optimization passes later.
        Returns: (success, ir_path, error_message)
        """
        fd, ir_file = tempfile.mkstemp(suffix='.ll')
        os.close(fd)
        try:
            utilities_dir = self._find_polybench_utilities(input_file)
            source_dir    = str(Path(input_file).parent)
            extra_args = [f'-I{source_dir}']
            if utilities_dir:
                extra_args += [f'-I{utilities_dir}']
                if (utilities_dir is not None
                        and any(n in input_file for n in self._POLYBENCH_DIR_NAMES)):
                    extra_args += ['-DPOLYBENCH_TIME', '-DLARGE_DATASET']

            if input_file.endswith('.c'):
                cmd = [
                    self.config.clang_path,
                    '-S', '-emit-llvm', '-O0',
                    '-Xclang', '-disable-O0-optnone',
                    '-std=c99',
                ] + extra_args + [input_file, '-o', ir_file]
            elif input_file.endswith('.cpp'):
                cmd = [
                    self.config.clang_cxx_path,
                    '-S', '-emit-llvm', '-O0',
                ] + extra_args + [input_file, '-o', ir_file]
            else:
                os.unlink(ir_file)
                return False, None, "Unsupported file type"

            result = subprocess.run(cmd, timeout=self.config.timeout_seconds,
                                    capture_output=True, text=True)
            if result.returncode != 0:
                try:
                    os.unlink(ir_file)
                except Exception:
                    pass
                return False, None, f"Clang -emit-llvm failed: {result.stderr[:300]}"
            return True, ir_file, None

        except subprocess.TimeoutExpired:
            try:
                os.unlink(ir_file)
            except Exception:
                pass
            return False, None, f"IR extraction timeout after {self.config.timeout_seconds}s"
        except Exception as e:
            try:
                os.unlink(ir_file)
            except Exception:
                pass
            return False, None, f"IR extraction error: {e}"
