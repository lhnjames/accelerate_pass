from dataclasses import dataclass, field
from typing import List, Dict, Optional
import hashlib


@dataclass
class Remark:
    id: str
    pass_name: str
    location: str
    reason: str
    remark_type: str  # 'Missed', 'Applied', 'Analysis'
    function: str
    occurrences: int = 1


@dataclass
class IRStats:
    instruction_count: int
    loop_count: int
    function_count: int
    before_instruction_count: Optional[int] = None


@dataclass
class RemarksData:
    missed: List[Remark] = field(default_factory=list)
    applied: Dict[str, int] = field(default_factory=dict)
    analysis: List[Remark] = field(default_factory=list)
    ir_stats: IRStats = field(default_factory=lambda: IRStats(0, 0, 0))


class RemarkIDGenerator:
    @classmethod
    def generate_id(cls, pass_name: str, location: str, reason: str = '') -> str:
        content = f"{pass_name}:{location}:{reason[:40]}"
        h = hashlib.md5(content.encode()).hexdigest()[:10]
        return f"M_{pass_name[:4]}_{h}"
