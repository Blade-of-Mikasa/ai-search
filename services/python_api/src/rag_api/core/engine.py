"""Application service that composes retrieval and evidence governance."""

from __future__ import annotations

from rag_api import __version__
from rag_api.core_client import (
    CoreHealth,
    CorePlanResult,
    IndexAssetCommand,
    IndexAssetResult,
)
from rag_api.domain import ExecutionPlan

from .evidence import Evidence, process_evidence
from .index import InMemoryIndex


class InMemoryCore:
    """Pure-Python replacement for the former C++ core process."""

    def __init__(self, index: InMemoryIndex | None = None) -> None:
        self._index = index or InMemoryIndex()

    async def health(self) -> CoreHealth:
        return CoreHealth(
            service="nano-ai-search-core", version=__version__, ready=True
        )

    async def execute_plan(self, plan: ExecutionPlan) -> CorePlanResult:
        errors = plan.validate()
        if errors:
            raise ValueError("; ".join(errors))

        evidence = [Evidence.from_external(item) for item in plan.external_evidence]
        route_error_codes: list[str] = []
        for route in plan.routes:
            try:
                evidence.extend(
                    self._index.search(
                        tenant_id=plan.tenant_id,
                        allowed_acl_ids=plan.allowed_acl_ids,
                        route=route,
                    )
                )
            except ValueError:
                route_error_codes.append(
                    f"{route.modality.name}_RETRIEVAL_FAILED"
                )

        return process_evidence(
            request_id=plan.request_id,
            evidence=evidence,
            context_token_budget=plan.context_token_budget,
            max_evidence_tokens=plan.max_evidence_tokens,
            route_error_codes=tuple(route_error_codes),
        )

    async def index_asset(self, command: IndexAssetCommand) -> IndexAssetResult:
        return self._index.index_asset(command)

    async def close(self) -> None:
        return None
