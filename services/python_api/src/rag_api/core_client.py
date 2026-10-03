"""Async HTTP/JSON client for the C++ Nano Core."""

from __future__ import annotations

from dataclasses import dataclass
import json
from math import isfinite
from typing import Any, Protocol

import aiohttp

from .domain import ExecutionPlan, Modality


class CoreUnavailableError(RuntimeError):
    """Raised when the C++ Core cannot be reached or violates its contract."""


@dataclass(frozen=True)
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


@dataclass(frozen=True)
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


class HttpCoreClient:
    """Maps typed Python values to the Nano Core HTTP/JSON endpoints."""

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
        try:
            return CoreHealth(
                service=_string(payload, "service"),
                version=_string(payload, "version"),
                ready=_bool(payload, "ready"),
            )
        except (KeyError, TypeError, ValueError) as error:
            raise CoreUnavailableError("Nano Core returned invalid health JSON") from error

    async def execute_plan(self, plan: ExecutionPlan) -> CorePlanResult:
        validation_errors = plan.validate()
        if validation_errors:
            raise ValueError("; ".join(validation_errors))
        payload = await self._request(
            "POST",
            "/v1/execute-plan",
            json_body={
                "request_id": plan.request_id,
                "user_id": plan.user_id,
                "conversation_id": plan.conversation_id,
                "tenant_id": plan.tenant_id,
                "allowed_acl_ids": list(plan.allowed_acl_ids),
                "routes": [
                    {
                        "route_id": route.route_id,
                        "query": route.query,
                        "source_scope": int(route.source_scope),
                        "modality": int(route.modality),
                        "top_k": route.top_k,
                        "timeout_ms": route.timeout_ms,
                        "dense_embedding": list(route.dense_embedding),
                        "embedding_model_id": route.embedding_model_id,
                        "embedding_model_version": route.embedding_model_version,
                    }
                    for route in plan.routes
                ],
                "external_evidence": [
                    {
                        "evidence_id": item.evidence_id,
                        "content": item.content,
                        "modality": int(item.modality),
                        "source_scope": int(item.source_scope),
                        "title": item.title,
                        "source": item.source,
                        "url": item.url,
                        "published_at_unix_ms": item.published_at_unix_ms,
                        "retrieved_at_unix_ms": item.retrieved_at_unix_ms,
                        "score": item.score,
                        "metadata": dict(item.metadata),
                        "content_sha256": item.content_sha256,
                    }
                    for item in plan.external_evidence
                ],
                "context_token_budget": plan.context_token_budget,
                "max_evidence_tokens": plan.max_evidence_tokens,
            },
            timeout=self._timeout,
        )
        try:
            evidence = _list(payload, "evidence")
            return CorePlanResult(
                request_id=_string(payload, "request_id"),
                context=_string(payload, "context"),
                evidence_count=len(evidence),
                citations=tuple(
                    CoreCitation(
                        citation_id=_int(item, "citation_id"),
                        evidence_id=_string(item, "evidence_id"),
                        source=_string(item, "source"),
                        url=_string(item, "url"),
                        title=_string(item, "title"),
                        modality=Modality(_int(item, "modality")),
                        metadata=tuple(sorted(_dict(item, "metadata").items())),
                    )
                    for item in _list(payload, "citations")
                ),
                conflicts=tuple(
                    CoreConflict(
                        evidence_ids=tuple(
                            _require_strings(item, "evidence_ids")
                        ),
                        type=_string(item, "type"),
                        reason=_string(item, "reason"),
                    )
                    for item in _list(payload, "conflicts")
                ),
                evidence_decisions=tuple(
                    CoreEvidenceDecision(
                        evidence_id=_string(item, "evidence_id"),
                        disposition=_string(item, "disposition"),
                        representative_evidence_id=_string(
                            item, "representative_evidence_id"
                        ),
                        reason=_string(item, "reason"),
                    )
                    for item in _list(payload, "evidence_decisions")
                ),
                context_token_count=_int(payload, "context_token_count"),
                context_truncated=_bool(payload, "context_truncated"),
                token_count_method=_string(payload, "token_count_method"),
                route_error_codes=tuple(
                    _string(item, "code")
                    for item in _list(payload, "route_errors")
                ),
                partial_failure=_bool(payload, "partial_failure"),
            )
        except (KeyError, TypeError, ValueError) as error:
            raise CoreUnavailableError(
                f"Nano Core returned invalid execute-plan JSON: {error}"
            ) from error

    async def index_asset(self, command: IndexAssetCommand) -> IndexAssetResult:
        self._validate_index_command(command)
        units = tuple(self._unit_payload(unit) for unit in command.units)
        batches = self._index_batches(command, units)
        indexed_unit_count = 0
        collection_alias = ""
        for batch_number, batch in enumerate(batches):
            response = await self._request(
                "POST",
                "/v1/index-asset",
                json_body=self._index_payload(
                    command,
                    batch,
                    append_to_asset_version=batch_number > 0,
                ),
                timeout=self._index_timeout,
            )
            try:
                response_request_id = _string(response, "request_id")
                response_asset_id = _string(response, "asset_id")
                response_asset_version = _int(response, "asset_version")
                response_count = _int(response, "indexed_unit_count")
                response_alias = _string(response, "collection_alias")
            except (KeyError, TypeError, ValueError) as error:
                raise CoreUnavailableError(
                    "Nano Core returned invalid index JSON"
                ) from error
            if (
                response_request_id != command.request_id
                or response_asset_id != command.asset_id
                or response_asset_version != command.asset_version
                or response_count != len(batch)
                or not response_alias
            ):
                raise CoreUnavailableError(
                    "Nano Core returned inconsistent index metadata"
                )
            if collection_alias and collection_alias != response_alias:
                raise CoreUnavailableError(
                    "Nano Core changed collection during batched indexing"
                )
            collection_alias = response_alias
            indexed_unit_count += response_count
        return IndexAssetResult(
            request_id=command.request_id,
            asset_id=command.asset_id,
            asset_version=command.asset_version,
            indexed_unit_count=indexed_unit_count,
            collection_alias=collection_alias,
        )

    async def _request(
        self,
        method: str,
        path: str,
        *,
        json_body: dict[str, Any] | None = None,
        timeout: aiohttp.ClientTimeout,
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
                        f"Nano Core returned non-JSON HTTP {response.status}"
                    ) from error
                if not isinstance(payload, dict):
                    raise CoreUnavailableError("Nano Core JSON must be an object")
                if not 200 <= response.status < 300:
                    detail = payload.get("error", {})
                    message = (
                        detail.get("message", "request rejected")
                        if isinstance(detail, dict)
                        else "request rejected"
                    )
                    if response.status in {400, 411, 412, 413, 422}:
                        raise ValueError(f"Nano Core rejected request: {message}")
                    raise CoreUnavailableError(
                        f"Nano Core HTTP {response.status}: {message}"
                    )
                return payload
        except (ValueError, CoreUnavailableError):
            raise
        except (aiohttp.ClientError, TimeoutError) as error:
            raise CoreUnavailableError(
                f"Nano Core HTTP request failed: {type(error).__name__}"
            ) from error

    async def _get_session(self) -> aiohttp.ClientSession:
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession(trust_env=False)
            self._owns_session = True
        return self._session

    @staticmethod
    def _unit_payload(unit: IndexUnit) -> dict[str, Any]:
        return {
            "unit_id": unit.unit_id,
            "modality": int(unit.modality),
            "content": unit.content,
            "title": unit.title,
            "ordinal": unit.ordinal,
            "page_number": unit.page_number,
            "content_sha256": unit.content_sha256,
            "dense_embedding": list(unit.dense_embedding),
            "embedding_model_id": unit.embedding_model_id,
            "embedding_model_version": unit.embedding_model_version,
            "metadata": dict(unit.metadata),
        }

    @staticmethod
    def _index_payload(
        command: IndexAssetCommand,
        units: tuple[dict[str, Any], ...],
        *,
        append_to_asset_version: bool,
    ) -> dict[str, Any]:
        return {
            "request_id": command.request_id,
            "tenant_id": command.tenant_id,
            "acl_id": command.acl_id,
            "asset_id": command.asset_id,
            "asset_version_id": command.asset_version_id,
            "asset_version": command.asset_version,
            "object_key": command.object_key,
            "units": list(units),
            "append_to_asset_version": append_to_asset_version,
        }

    def _index_batches(
        self,
        command: IndexAssetCommand,
        units: tuple[dict[str, Any], ...],
    ) -> tuple[tuple[dict[str, Any], ...], ...]:
        batches: list[tuple[dict[str, Any], ...]] = []
        current: list[dict[str, Any]] = []
        for unit in units:
            current.append(unit)
            payload = self._index_payload(
                command, tuple(current), append_to_asset_version=True
            )
            if _json_size(payload) <= self._index_batch_max_bytes:
                continue
            current.pop()
            if not current:
                raise ValueError("one normalized unit exceeds the HTTP batch limit")
            batches.append(tuple(current))
            current = [unit]
            payload = self._index_payload(
                command, tuple(current), append_to_asset_version=True
            )
            if _json_size(payload) > self._index_batch_max_bytes:
                raise ValueError("one normalized unit exceeds the HTTP batch limit")
        if current:
            batches.append(tuple(current))
        return tuple(batches)

    @staticmethod
    def _validate_index_command(command: IndexAssetCommand) -> None:
        if (
            not command.request_id
            or not command.tenant_id
            or not command.acl_id
            or not command.asset_id
            or not command.asset_version_id
            or command.asset_version <= 0
            or not command.object_key
            or not command.units
        ):
            raise ValueError("index asset identity and units must not be empty")
        first = command.units[0]
        if first.modality not in {
            Modality.DOCUMENT,
            Modality.IMAGE,
            Modality.VIDEO,
        }:
            raise ValueError("index unit modality must be document, image, or video")
        if first.modality is Modality.IMAGE and len(command.units) != 1:
            raise ValueError("one image asset version must contain exactly one unit")
        unit_ids: set[str] = set()
        ordinals: set[int] = set()
        for unit in command.units:
            if (
                not unit.unit_id
                or not unit.content
                or len(unit.content.encode("utf-8")) > 65_535
                or len(unit.title.encode("utf-8")) > 2_048
                or unit.ordinal < 0
                or unit.page_number < 0
                or len(unit.content_sha256) != 64
                or not unit.dense_embedding
                or any(not isfinite(value) for value in unit.dense_embedding)
                or not unit.embedding_model_id
                or not unit.embedding_model_version
            ):
                raise ValueError("index unit content and embedding must be valid")
            if (
                unit.modality != first.modality
                or unit.embedding_model_id != first.embedding_model_id
                or unit.embedding_model_version != first.embedding_model_version
                or len(unit.dense_embedding) != len(first.dense_embedding)
            ):
                raise ValueError(
                    "index units must share one modality and embedding model schema"
                )
            metadata = dict(unit.metadata)
            if len(metadata) != len(unit.metadata) or any(
                not key
                or len(key.encode("utf-8")) > 128
                or len(value.encode("utf-8")) > 60_000
                for key, value in unit.metadata
            ):
                raise ValueError("index unit metadata must be unique and bounded")
            if unit.modality is Modality.IMAGE:
                _validate_image_metadata(unit, metadata)
            if unit.modality is Modality.VIDEO:
                _validate_video_metadata(unit, metadata)
            if unit.unit_id in unit_ids or unit.ordinal in ordinals:
                raise ValueError("index unit IDs and ordinals must be unique")
            unit_ids.add(unit.unit_id)
            ordinals.add(unit.ordinal)

    async def close(self) -> None:
        if self._session is not None and self._owns_session:
            await self._session.close()
        self._session = None


def _validate_image_metadata(unit: IndexUnit, metadata: dict[str, str]) -> None:
    width = metadata.get("width", "")
    height = metadata.get("height", "")
    if (
        not unit.title
        or metadata.get("media_type")
        not in {"image/jpeg", "image/png", "image/webp"}
        or not width.isascii()
        or not width.isdecimal()
        or not height.isascii()
        or not height.isdecimal()
        or not 1 <= int(width) <= 4_294_967_295
        or not 1 <= int(height) <= 4_294_967_295
        or not metadata.get("vision_model_id")
        or not metadata.get("vision_model_version")
    ):
        raise ValueError("image index metadata and caption must be valid")


def _validate_video_metadata(unit: IndexUnit, metadata: dict[str, str]) -> None:
    names = (
        "duration_ms",
        "width",
        "height",
        "start_ms",
        "end_ms",
        "keyframe_ms",
    )
    values = {name: metadata.get(name, "") for name in names}
    if any(not value.isascii() or not value.isdecimal() for value in values.values()):
        raise ValueError("video timestamps and dimensions must be integers")
    duration = int(values["duration_ms"])
    start = int(values["start_ms"])
    end = int(values["end_ms"])
    keyframe = int(values["keyframe_ms"])
    if (
        not unit.title
        or metadata.get("media_type")
        not in {"video/mp4", "video/quicktime", "video/webm"}
        or not 1 <= duration <= 86_400_000
        or not 1 <= int(values["width"]) <= 32_768
        or not 1 <= int(values["height"]) <= 32_768
        or not 0 <= start < end <= duration
        or not start <= keyframe < end
        or not metadata.get("caption")
        or not metadata.get("speech_model_id")
        or not metadata.get("speech_model_version")
        or not metadata.get("vision_model_id")
        or not metadata.get("vision_model_version")
    ):
        raise ValueError("video index metadata and segment bounds are invalid")


def _json_size(payload: dict[str, Any]) -> int:
    return len(
        json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode(
            "utf-8"
        )
    )


def _string(payload: dict[str, Any], key: str) -> str:
    value = payload[key]
    if not isinstance(value, str):
        raise TypeError(f"{key} must be a string")
    return value


def _int(payload: dict[str, Any], key: str) -> int:
    value = payload[key]
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{key} must be an integer")
    return value


def _bool(payload: dict[str, Any], key: str) -> bool:
    value = payload[key]
    if not isinstance(value, bool):
        raise TypeError(f"{key} must be a boolean")
    return value


def _list(payload: dict[str, Any], key: str) -> list[dict[str, Any]]:
    value = payload[key]
    if not isinstance(value, list) or any(not isinstance(item, dict) for item in value):
        raise TypeError(f"{key} must be a list of objects")
    return value


def _dict(payload: dict[str, Any], key: str) -> dict[str, str]:
    value = payload[key]
    if not isinstance(value, dict) or any(
        not isinstance(item_key, str) or not isinstance(item_value, str)
        for item_key, item_value in value.items()
    ):
        raise TypeError(f"{key} must map strings to strings")
    return value


def _require_strings(payload: dict[str, Any], key: str) -> list[str]:
    value = payload[key]
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise TypeError(f"{key} must be a list of strings")
    return value
