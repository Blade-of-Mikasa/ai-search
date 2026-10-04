"""Provider-neutral web search and extraction pipeline."""

from .domain import (
    ExtractionStatus,
    ExtractedPage,
    FetchedPage,
    SearchCitation,
    SearchProvider,
    SearchProviderError,
    SearchQuery,
    SearchResponse,
    SourceTime,
    SourceTimeKind,
    WebExtractionError,
    WebFetchError,
    WebSearchBundle,
    WebSource,
)
from .service import WebSearchService
from .evidence import web_bundle_to_evidence
from .tavily import TavilySearchProvider

__all__ = [
    "ExtractionStatus",
    "ExtractedPage",
    "FetchedPage",
    "SearchCitation",
    "SearchProvider",
    "SearchProviderError",
    "SearchQuery",
    "SearchResponse",
    "SourceTime",
    "SourceTimeKind",
    "WebExtractionError",
    "WebFetchError",
    "WebSearchBundle",
    "WebSearchService",
    "WebSource",
    "TavilySearchProvider",
    "web_bundle_to_evidence",
]
