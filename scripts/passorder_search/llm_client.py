"""Minimal DeepSeek chat client for the pass-order-search baseline -- uses
the SAME external model (DeepSeek) as the OpenCode baseline for an
apples-to-apples "which LLM" comparison, but called directly via the OpenAI-
compatible API instead of through OpenCode's agent/tool-use loop, since this
harness only needs one structured JSON answer per round, not a coding agent.
"""
import json
import os
import re
from pathlib import Path
from openai import OpenAI

_ENV_PATH = Path("/home/hanning/comet/.env")


def _load_api_key() -> str:
    if os.environ.get("DEEPSEEK_API_KEY"):
        return os.environ["DEEPSEEK_API_KEY"]
    for line in _ENV_PATH.read_text().splitlines():
        if line.startswith("DEEPSEEK_API_KEY="):
            return line.split("=", 1)[1].strip()
    raise RuntimeError("DEEPSEEK_API_KEY not found in env or .env")


_client = OpenAI(api_key=_load_api_key(), base_url="https://api.deepseek.com")
MODEL = "deepseek-chat"


def ask_json(system: str, user: str, max_tokens: int = 800) -> dict:
    """Call DeepSeek, extract the first {...} JSON object from the reply."""
    resp = _client.chat.completions.create(
        model=MODEL,
        messages=[{"role": "system", "content": system},
                  {"role": "user", "content": user}],
        max_tokens=max_tokens,
        temperature=0.7,
    )
    content = resp.choices[0].message.content or ""
    m = re.search(r"\{.*\}", content, re.DOTALL)
    if not m:
        return {}
    try:
        return json.loads(m.group(0))
    except Exception:
        return {}
