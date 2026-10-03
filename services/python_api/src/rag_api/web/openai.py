"""OpenAI Responses web-search adapter for the Nano default path."""

from __future__ import annotations

from contextlib import asynccontextmanager
from typing import AsyncIterator

import aiohttp

from .bing import _parse_response
from .domain import SearchProviderError, SearchQuery, SearchResponse


class OpenAIWebSearchProvider:
    provider_name = "openai_responses_web_search"

    def __init__(
        self,
        *,
        responses_url: str,
        model_id: str,
        timeout_seconds: float,
        api_key: str | None = None,
        session: aiohttp.ClientSession | None = None,
    ) -> None:
        if not responses_url.startswith(("http://", "https://")):
            raise ValueError("Responses URL must use HTTP or HTTPS")
        if not model_id.strip():
            raise ValueError("web search model ID must not be blank")
        self._responses_url = responses_url.rstrip("/")
        self._model_id = model_id
        self._timeout = aiohttp.ClientTimeout(total=timeout_seconds)
        self._api_key = api_key
        self._session = session

    async def search(self, query: SearchQuery) -> SearchResponse:
        request = {
            "model": self._model_id,
            "input": query.text,
            "tool_choice": "required",
            "tools": [
                {
                    "type": "web_search",
                    "search_context_size": "low",
                }
            ],
        }
        headers = {"Content-Type": "application/json"}
        if self._api_key:
            headers["Authorization"] = f"Bearer {self._api_key}"
        try:
            async with self._session_scope() as session:
                async with session.post(
                    self._responses_url,
                    json=request,
                    headers=headers,
                    timeout=self._timeout,
                ) as response:
                    if not 200 <= response.status < 300:
                        detail = (await response.text())[:500]
                        retryable = response.status in {408, 409, 425, 429} or (
                            response.status >= 500
                        )
                        raise SearchProviderError(
                            f"Responses web search returned {response.status}: {detail}",
                            retryable=retryable,
                        )
                    payload = await response.json(content_type=None)
        except SearchProviderError:
            raise
        except (aiohttp.ClientError, TimeoutError) as error:
            raise SearchProviderError(
                f"Responses web search is unavailable: {type(error).__name__}",
                retryable=True,
            ) from error
        except ValueError as error:
            raise SearchProviderError(
                "Responses web search returned invalid JSON", retryable=False
            ) from error
        return _parse_response(
            payload,
            query.text,
            self.provider_name,
            max_results=query.count,
        )

    @asynccontextmanager
    async def _session_scope(self) -> AsyncIterator[aiohttp.ClientSession]:
        if self._session is not None:
            yield self._session
            return
        async with aiohttp.ClientSession(trust_env=False) as session:
            yield session
