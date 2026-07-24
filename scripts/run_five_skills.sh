#!/usr/bin/env bash
# Skills-on counterpart to run_five_normal.sh; keeps a separate log root.
set -euo pipefail
PROJECT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$PROJECT_ROOT"
export LOG_ROOT="${LOG_ROOT:-logs_five_skills/$(date -u +%Y%m%dT%H%M%SZ)}"
exec env SKILLS_OFF=0 scripts/run_five_normal.sh "$@"
