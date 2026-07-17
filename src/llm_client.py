import logging
import json
from typing import Dict, Any, Optional, List
from openai import OpenAI, OpenAIError

from src.config import LLMConfig

logger = logging.getLogger(__name__)


def strip_json_fences(text: str) -> str:
    """Strip a leading ```json/``` code fence and trailing ``` from LLM output."""
    clean = text.strip()
    for fence in ("```json", "```"):
        if clean.startswith(fence):
            clean = clean[len(fence):]
    if clean.endswith("```"):
        clean = clean[:-3]
    return clean.strip()


class LLMClient:
    def __init__(self, config: LLMConfig):
        self.config = config
        self.client = OpenAI(
            api_key=config.api_key,
            base_url=config.base_url.rstrip('/'),
            timeout=config.timeout_seconds,
            max_retries=3,
        )
        self.call_count = 0

    def _completion_kwargs(self,
                           messages: List[Dict[str, str]],
                           temperature: Optional[float],
                           max_tokens: Optional[int]) -> Dict[str, Any]:
        kwargs: Dict[str, Any] = {
            'model': self.config.model,
            'messages': messages,
            'temperature': self.config.temperature if temperature is None else temperature,
            'max_tokens': self.config.max_tokens if max_tokens is None else max_tokens,
            'stream': False,
        }
        if self.config.reasoning_effort:
            kwargs['reasoning_effort'] = self.config.reasoning_effort
        if self.config.thinking_enabled:
            kwargs['extra_body'] = {'thinking': {'type': 'enabled'}}
        return kwargs

    def health_check(self) -> tuple:
        if not self.config.api_key:
            return False, "missing API key"
        try:
            response = self.client.chat.completions.create(
                **self._completion_kwargs(
                    [{'role': 'user', 'content': 'Reply with OK.'}],
                    temperature=0,
                    max_tokens=4,
                )
            )
            if response.choices:
                return True, "ok"
            return False, f"unexpected response: {str(response)[:200]}"
        except OpenAIError as e:
            return False, f"API error: {e}"
        except Exception as e:
            return False, f"unexpected error: {e}"

    def call(self,
             messages: List[Dict[str, str]],
             temperature: Optional[float] = None,
             max_tokens: Optional[int] = None,
             timeout: Optional[int] = None) -> Optional[str]:
        if not self.config.api_key:
            logger.error("No API key configured")
            return None

        try:
            for attempt in range(1, 4):
                kwargs = self._completion_kwargs(messages, temperature, max_tokens)
                if timeout is not None:
                    kwargs['timeout'] = timeout
                response = self.client.chat.completions.create(**kwargs)
                self.call_count += 1

                if response.choices:
                    msg = response.choices[0].message
                    content = msg.content or ''
                    if not content.strip():
                        # Some reasoning models put the answer in reasoning_content
                        # when content is empty (API flakiness / thinking overflow)
                        rc = getattr(msg, 'reasoning_content', None) or ''
                        if rc.strip():
                            logger.warning(f"content empty, falling back to reasoning_content on attempt {attempt}")
                            return rc.strip()
                        logger.warning(f"Empty LLM response on attempt {attempt}")
                        continue
                    return content

                logger.error(f"Unexpected response format: {response}")
                if attempt >= 3:
                    return None

            return None

        except OpenAIError as e:
            logger.error(f"API request failed: {e}")
            return None
        except Exception as e:
            logger.error(f"Unexpected error: {e}")
            return None

    def parse_json_response(self, response: str) -> Optional[Dict]:
        try:
            return json.loads(strip_json_fences(response))
        except Exception as e:
            logger.error(f"Failed to parse JSON response: {e}")
            return None
