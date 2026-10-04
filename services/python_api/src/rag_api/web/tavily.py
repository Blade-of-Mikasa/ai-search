"""Tavily Search adapter for source-bearing web retrieval."""

from __future__ import annotations

from contextlib import asynccontextmanager
from typing import Any, AsyncIterator, Literal
from urllib.parse import urldefrag, urlparse, urlsplit

import aiohttp

from .domain import (
    SearchCitation,
    SearchProviderError,
    SearchQuery,
    SearchResponse,
)


TavilySearchDepth = Literal["basic", "fast", "ultra-fast", "advanced"]


class TavilySearchProvider:
    provider_name = "tavily"

    def __init__(
        self,
        *,
        search_url: str,
        api_key: str,
        timeout_seconds: float,
        search_depth: TavilySearchDepth = "basic",
        session: aiohttp.ClientSession | None = None,
    ) -> None:
        parsed = urlparse(search_url)
        if (
            parsed.scheme != "https"
            or not parsed.netloc
            or parsed.username is not None
            or parsed.password is not None
        ):
            raise ValueError("Tavily search URL must be an HTTPS URL")
        if not api_key.strip():
            raise ValueError("Tavily API key must not be blank")
        if search_depth not in {"basic", "fast", "ultra-fast", "advanced"}:
            raise ValueError("unsupported Tavily search depth")
        self._search_url = search_url.rstrip("/")
        self._api_key = api_key
        self._timeout = aiohttp.ClientTimeout(total=timeout_seconds)
        self._search_depth = search_depth
        self._session = session

    async def search(self, query: SearchQuery) -> SearchResponse:
        request: dict[str, Any] = {
            "query": query.text,
            "search_depth": self._search_depth,
            "max_results": min(query.count, 20),
            "include_answer": False,
            "include_raw_content": False,
            "include_images": False,
            "include_published_date": True,
        }
        if query.language is not None:
            request["language"] = query.language
            request["filter_by_language"] = True
        if query.freshness in {"day", "week", "month"}:
            request["time_range"] = query.freshness
        elif query.freshness is not None:
            request["start_date"], request["end_date"] = query.freshness.split("..")

        try:
            async with self._session_scope() as session:
                async with session.post(
                    self._search_url,
                    json=request,
                    headers={
                        "Authorization": f"Bearer {self._api_key}",
                        "Content-Type": "application/json",
                    },
                    timeout=self._timeout,
                ) as response:
                    if not 200 <= response.status < 300:
                        detail = (await response.text())[:500]
                        retryable = response.status in {408, 409, 425, 429} or (
                            response.status >= 500
                        )
                        raise SearchProviderError(
                            f"Tavily search returned {response.status}: {detail}",
                            retryable=retryable,
                        )
                    payload = await response.json(content_type=None)
        except SearchProviderError:
            raise
        except (aiohttp.ClientError, TimeoutError) as error:
            raise SearchProviderError(
                f"Tavily search is unavailable: {type(error).__name__}",
                retryable=True,
            ) from error
        except ValueError as error:
            raise SearchProviderError(
                "Tavily search returned invalid JSON", retryable=False
            ) from error
        return _parse_response(payload, query.text, max_results=query.count)

    @asynccontextmanager
    async def _session_scope(self) -> AsyncIterator[aiohttp.ClientSession]:
        if self._session is not None:
            yield self._session
            return
        async with aiohttp.ClientSession(trust_env=False) as session:
            yield session


def _parse_response(
    payload: Any,
    query: str,
    *,
    max_results: int,
) -> SearchResponse:
    if not isinstance(payload, dict):
        raise SearchProviderError(
            "Tavily search response must be an object", retryable=False
        )
    results = payload.get("results")
    if not isinstance(results, list):
        raise SearchProviderError(
            "Tavily search response contains no results", retryable=False
        )

    citations: list[SearchCitation] = []
    seen: set[str] = set()
    for item in results:
        if not isinstance(item, dict):
            continue
        url = item.get("url")
        if not _is_http_url(url):
            continue
        identity = urldefrag(url)[0]
        if identity in seen:
            continue
        title = item.get("title")
        if not isinstance(title, str) or not title.strip():
            title = urlsplit(url).hostname or url
        content = item.get("content")
        if not isinstance(content, str):
            content = ""
        citations.append(
            SearchCitation(
                rank=len(citations) + 1,
                url=url,
                title=title.strip()[:4_096],
                cited_text=content.strip()[:16_384],
            )
        )
        seen.add(identity)
        if len(citations) >= max_results:
            break

    if not citations:
        raise SearchProviderError(
            "Tavily search response contains no usable results", retryable=False
        )
    return SearchResponse(
        provider=TavilySearchProvider.provider_name,
        query=query,
        citations=tuple(citations),
        grounded_text="\n".join(
            citation.cited_text for citation in citations if citation.cited_text
        ),
    )


def _is_http_url(value: Any) -> bool:
    if not isinstance(value, str) or len(value) > 16_384:
        return False
    try:
        parsed = urlsplit(value)
        parsed.port
    except ValueError:
        return False
    return (
        parsed.scheme.lower() in {"http", "https"}
        and parsed.hostname is not None
        and parsed.username is None
        and parsed.password is None
        and not any(character in value for character in ("\r", "\n", "\x00"))
    )
