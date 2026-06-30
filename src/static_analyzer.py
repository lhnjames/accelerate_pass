import subprocess
import tempfile
import os
import yaml
import logging
from typing import List, Dict, Optional, Tuple
from pathlib import Path

from src.data_structures import Remark, RemarksData, IRStats, RemarkIDGenerator
from src.pass_utils import build_opt_command

logger = logging.getLogger(__name__)


class RemarksExtractor:
    def __init__(self, opt_path: str):
        self.opt_path = opt_path

    def extract_remarks(self,
                        ir_file: str,
                        passes: Optional[List[str]] = None,
                        timeout: int = 300) -> Tuple[bool, Optional[RemarksData], Optional[str]]:
        """
        Run opt -O3 with remark flags and parse the YAML output.
        Returns: (success, remarks_data, error_message)
        """
        fd_rem, remarks_file = tempfile.mkstemp(suffix='.yaml')
        os.close(fd_rem)
        fd_ir, output_ir = tempfile.mkstemp(suffix='.ll')
        os.close(fd_ir)

        try:
            if passes:
                cmd = build_opt_command(
                    self.opt_path, ir_file, output_ir, passes,
                    extra_opt_args=[
                        '-pass-remarks-output=' + remarks_file,
                        '-pass-remarks=.*',
                        '-pass-remarks-missed=.*',
                        '-pass-remarks-analysis=.*',
                    ],
                )
            else:
                cmd = [
                    self.opt_path, '-O3',
                    '-pass-remarks-output=' + remarks_file,
                    '-pass-remarks=.*',
                    '-pass-remarks-missed=.*',
                    '-pass-remarks-analysis=.*',
                    ir_file, '-o', output_ir,
                ]

            result = subprocess.run(cmd, timeout=timeout, capture_output=True, text=True)
            if result.returncode != 0:
                err = result.stderr[:300] if result.stderr else f"exit {result.returncode}"
                return False, None, f"opt failed: {err}"

            return True, self._parse_remarks_file(remarks_file), None

        except subprocess.TimeoutExpired:
            return False, None, f"Timeout after {timeout}s"
        except Exception as e:
            return False, None, f"Error: {e}"
        finally:
            Path(remarks_file).unlink(missing_ok=True)
            Path(output_ir).unlink(missing_ok=True)

    def _parse_remarks_file(self, remarks_file: str) -> RemarksData:
        data = RemarksData()
        try:
            path = Path(remarks_file)
            if not path.exists() or not path.stat().st_size:
                return data

            with open(remarks_file) as f:
                content = f.read()

            loader_class = yaml.SafeLoader

            def _tag_constructor(loader, tag_suffix, node):
                d = loader.construct_mapping(node, deep=True)
                d['!'] = tag_suffix
                return d

            yaml.add_multi_constructor('!', _tag_constructor, Loader=loader_class)

            applied_dict: Dict[str, int] = {}
            for doc in yaml.load_all(content, Loader=loader_class):
                if not doc or not isinstance(doc, dict):
                    continue
                remark_type = doc.get('!', 'Unknown')
                pass_name   = doc.get('Pass', '')
                function    = doc.get('Function', '')
                debug_loc   = doc.get('DebugLoc', {}) or {}
                location    = f"{debug_loc.get('File', '')}:{debug_loc.get('Line', 0)}"
                reason      = self._extract_reason(doc.get('Args', []))

                remark = Remark(
                    id=RemarkIDGenerator.generate_id(pass_name, location, reason),
                    pass_name=pass_name,
                    location=location,
                    reason=reason,
                    remark_type=remark_type,
                    function=function,
                )
                if remark_type == 'Missed':
                    data.missed.append(remark)
                elif remark_type == 'Passed':
                    applied_dict[pass_name] = applied_dict.get(pass_name, 0) + 1
                elif remark_type == 'Analysis':
                    data.analysis.append(remark)

            data.applied = applied_dict
        except Exception as e:
            logger.error(f"Error parsing remarks: {e}")
        return data

    @staticmethod
    def _extract_reason(args: List) -> str:
        parts = []
        for arg in args:
            if isinstance(arg, dict) and 'String' in arg:
                parts.append(arg['String'])
            elif isinstance(arg, str):
                parts.append(arg)
        return ''.join(parts).strip()
