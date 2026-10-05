"""Python surface package for the multimodal RAG system."""

from .domain import ExecutionPlan, Modality, RetrievalRoute, SourceScope

__version__ = "0.3.0"

__all__ = [
    "ExecutionPlan",
    "Modality",
    "RetrievalRoute",
    "SourceScope",
    "__version__",
]
