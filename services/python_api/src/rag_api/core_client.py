"""Typed HTTP client for the Python evidence and in-memory index service."""

from __future__ import annotations

from dataclasses import dataclass, replace
import json
from typing import Any, Protocol, TypeVar

import aiohttp
from pydantic import TypeAdapter, ValidationError

from .domain import ExecutionPlan, Modality


class CoreUnavailableError(RuntimeError):
    """Raised when the core service cannot be reached or breaks its contract."""


@dataclass(frozen=True, slots=True)
class CoreHealth:
    service: str
    version: str
    ready: bool


@dataclass(frozen=True, slots=True)
class CoreCitation:
    citation_id: int
    evidence_id: str
    source: str
    url: str
    title: str
    modality: Modality
    metadata: tuple[tuple[str, str], ...]


@dataclass(frozen=True, slots=True)
class CoreConflict:
    evidence_ids: tuple[str, ...]
    type: str
    reason: str


@dataclass(frozen=True, slots=True)
class CoreEvidenceDecision:
    evidence_id: str
    disposition: str
    representative_evidence_id: str
    reason: str


@dataclass(frozen=True, slots=True)
class CorePlanResult:
    request_id: str
    context: str
    evidence_count: int
    citations: tuple[CoreCitation, ...]
    conflicts: tuple[CoreConflict, ...]
    evidence_decisions: tuple[CoreEvidenceDecision, ...]
    context_token_count: int
    context_truncated: bool
    token_count_method: str
    route_error_codes: tuple[str, ...]
    partial_failure: bool


@dataclass(frozen=True, slots=True)
class IndexUnit:
    unit_id: str
    modality: Modality
    content: str
    title: str
    ordinal: int
    page_number: int
    content_sha256: str
    dense_embedding: tuple[float, ...]
    embedding_model_id: str
    embedding_model_version: str
    metadata: tuple[tuple[str, str], ...] = ()


@dataclass(frozen=True, slots=True)
class IndexAssetCommand:
    request_id: str
    tenant_id: str
    acl_id: str
    asset_id: str
    asset_version_id: str
    asset_version: int
    object_key: str
    units: tuple[IndexUnit, ...]
    append_to_asset_version: bool = False


@dataclass(frozen=True, slots=True)
class IndexAssetResult:
    request_id: str
    asset_id: str
    asset_version: int
    indexed_unit_count: int
    collection_alias: str


class CoreClient(Protocol):
    async def health(self) -> CoreHealth: ...

    async def execute_plan(self, plan: ExecutionPlan) -> CorePlanResult: ...

    async def index_asset(self, command: IndexAssetCommand) -> IndexAssetResult: ...

    async def close(self) -> None: ...


ResponseT = TypeVar("ResponseT")
HEALTH_ADAPTER = TypeAdapter(CoreHealth)
PLAN_ADAPTER = TypeAdapter(ExecutionPlan)
PLAN_RESULT_ADAPTER = TypeAdapter(CorePlanResult)
INDEX_COMMAND_ADAPTER = TypeAdapter(IndexAssetCommand)
INDEX_RESULT_ADAPTER = TypeAdapter(IndexAssetResult)


class HttpCoreClient:
    """Maps shared Python contracts onto the core service's JSON endpoints."""

    def __init__(
        self,
        base_url: str,
        timeout_seconds: float = 2.0,
        index_timeout_seconds: float = 60.0,
        index_batch_max_bytes: int = 4_000_000,
        session: aiohttp.ClientSession | None = None,
    ) -> None:
        if not base_url.startswith(("http://", "https://")):
            raise ValueError("core base URL must use HTTP or HTTPS")
        if not 65_536 <= index_batch_max_bytes <= 7_000_000:
            raise ValueError(
                "index_batch_max_bytes must be between 65536 and 7000000"
            )
        self._base_url = base_url.rstrip("/")
        self._timeout = aiohttp.ClientTimeout(total=timeout_seconds)
        self._index_timeout = aiohttp.ClientTimeout(total=index_timeout_seconds)
        self._index_batch_max_bytes = index_batch_max_bytes
        self._session = session
        self._owns_session = session is None

    async def health(self) -> CoreHealth:
        payload = await self._request("GET", "/health", timeout=self._timeout)
        return _parse_response(HEALTH_ADAPTER, payload, "health")

    async def execute_plan(self, plan: ExecutionPlan) -> CorePlanResult:
        errors = plan.validate()
        if errors:
            raise ValueError("; ".join(errors))
        payload = await self._request(
            "POST",
            "/v1/execute-plan",
            json_body=PLAN_ADAPTER.dump_python(plan, mode="json"),
            timeout=self._timeout,
        )
        return _parse_response(PLAN_RESULT_ADAPTER, payload, "execute-plan")

    async def index_asset(self, command: IndexAssetCommand) -> IndexAssetResult:
        batches = _index_batches(command, self._index_batch_max_bytes)
        indexed_count = 0
        collection_alias = ""
        for batch_number, units in enumerate(batches):
            batch = _with_units(
                command,
                units,
                append_to_asset_version=(
                    command.append_to_asset_version or batch_number > 0
                ),
            )
            payload = await self._request(
                "POST",
                "/v1/index-asset",
                json_body=INDEX_COMMAND_ADAPTER.dump_python(batch, mode="json"),
                timeout=self._index_timeout,
            )
            result = _parse_response(INDEX_RESULT_ADAPTER, payload, "index-asset")
            if (
                result.request_id != command.request_id
                or result.asset_id != command.asset_id
                or result.asset_version != command.asset_version
                or result.indexed_unit_count != len(units)
            ):
                raise CoreUnavailableError(
                    "core service returned inconsistent index metadata"
                )
            if collection_alias and collection_alias != result.collection_alias:
                raise CoreUnavailableError(
                    "core service changed collection during batched indexing"
                )
            collection_alias = result.collection_alias
            indexed_count += result.indexed_unit_count
        return IndexAssetResult(
            request_id=command.request_id,
            asset_id=command.asset_id,
            asset_version=command.asset_version,
            indexed_unit_count=indexed_count,
            collection_alias=collection_alias,
        )

    async def _request(
        self,
        method: str,
        path: str,
        *,
        timeout: aiohttp.ClientTimeout,
        json_body: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        session = await self._get_session()
        try:
            async with session.request(
                method,
                f"{self._base_url}{path}",
                json=json_body,
                timeout=timeout,
            ) as response:
                try:
                    payload = await response.json(content_type=None)
                except (json.JSONDecodeError, ValueError, TypeError) as error:
                    raise CoreUnavailableError(
                        f"core service returned non-JSON HTTP {response.status}"
                    ) from error
                if not isinstance(payload, dict):
                    raise CoreUnavailableError("core service JSON must be an object")
                if 200 <= response.status < 300:
                    return payload
                detail = payload.get("detail", "request rejected")
                message = detail if isinstance(detail, str) else str(detail)
                if response.status in {400, 411, 412, 413, 422}:
                    raise ValueError(f"core service rejected request: {message}")
                raise CoreUnavailableError(
                    f"core service HTTP {response.status}: {message}"
                )
        except (ValueError, CoreUnavailableError):
            raise
        except (aiohttp.ClientError, TimeoutError) as error:
            raise CoreUnavailableError(
                f"core service request failed: {type(error).__name__}"
            ) from error

    async def _get_session(self) -> aiohttp.ClientSession:
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession(trust_env=False)
            self._owns_session = True
        return self._session

    async def close(self) -> None:
        if self._session is not None and self._owns_session:
            await self._session.close()
        self._session = None


def _parse_response(
    adapter: TypeAdapter[ResponseT], payload: dict[str, Any], endpoint: str
) -> ResponseT:
    try:
        return adapter.validate_python(payload)
    except ValidationError as error:
        raise CoreUnavailableError(
            f"core service returned invalid {endpoint} JSON"
        ) from error


def _index_batches(
    command: IndexAssetCommand, max_bytes: int
) -> tuple[tuple[IndexUnit, ...], ...]:
    if not command.units:
        raise ValueError("index asset units must not be empty")
    batches: list[tuple[IndexUnit, ...]] = []
    current: list[IndexUnit] = []
    for unit in command.units:
        current.append(unit)
        candidate = _with_units(
            command,
            tuple(current),
            append_to_asset_version=True,
        )
        if len(INDEX_COMMAND_ADAPTER.dump_json(candidate)) <= max_bytes:
            continue
        current.pop()
        if not current:
            raise ValueError("one normalized unit exceeds the HTTP batch limit")
        batches.append(tuple(current))
        current = [unit]
        single = _with_units(
            command,
            tuple(current),
            append_to_asset_version=True,
        )
        if len(INDEX_COMMAND_ADAPTER.dump_json(single)) > max_bytes:
            raise ValueError("one normalized unit exceeds the HTTP batch limit")
    if current:
        batches.append(tuple(current))
    return tuple(batches)


def _with_units(
    command: IndexAssetCommand,
    units: tuple[IndexUnit, ...],
    *,
    append_to_asset_version: bool,
) -> IndexAssetCommand:
    return replace(
        command,
        units=units,
        append_to_asset_version=append_to_asset_version,
    )
