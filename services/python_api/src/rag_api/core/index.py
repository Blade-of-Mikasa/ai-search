"""Small, process-local multimodal index with hybrid dense/lexical ranking."""

from __future__ import annotations

from dataclasses import dataclass
from math import isfinite, sqrt
import re
from threading import RLock

from rag_api.core_client import IndexAssetCommand, IndexAssetResult, IndexUnit
from rag_api.domain import Modality, RetrievalRoute, SourceScope

from .evidence import Evidence


SUPPORTED_IMAGE_TYPES = {"image/jpeg", "image/png", "image/webp"}
SUPPORTED_VIDEO_TYPES = {"video/mp4", "video/quicktime", "video/webm"}
UINT32_MAX = 4_294_967_295


@dataclass(frozen=True, slots=True)
class IndexedUnit:
    tenant_id: str
    acl_id: str
    asset_id: str
    asset_version_id: str
    asset_version: int
    object_key: str
    unit: IndexUnit


class InMemoryIndex:
    """Thread-safe index shared by all requests in one core process."""

    def __init__(self) -> None:
        self._units: dict[tuple[Modality, str], IndexedUnit] = {}
        self._lock = RLock()

    def index_asset(self, command: IndexAssetCommand) -> IndexAssetResult:
        validate_index_command(command)
        first = command.units[0]
        indexed = tuple(
            IndexedUnit(
                tenant_id=command.tenant_id,
                acl_id=command.acl_id,
                asset_id=command.asset_id,
                asset_version_id=command.asset_version_id,
                asset_version=command.asset_version,
                object_key=command.object_key,
                unit=unit,
            )
            for unit in command.units
        )
        with self._lock:
            if not command.append_to_asset_version:
                self._units = {
                    key: item
                    for key, item in self._units.items()
                    if not (
                        item.tenant_id == command.tenant_id
                        and item.asset_version_id == command.asset_version_id
                        and item.unit.modality is first.modality
                    )
                }
            self._units.update(
                {
                    (item.unit.modality, item.unit.unit_id): item
                    for item in indexed
                }
            )
        return IndexAssetResult(
            request_id=command.request_id,
            asset_id=command.asset_id,
            asset_version=command.asset_version,
            indexed_unit_count=len(indexed),
            collection_alias=_collection_alias(first),
        )

    def search(
        self,
        *,
        tenant_id: str,
        allowed_acl_ids: tuple[str, ...],
        route: RetrievalRoute,
    ) -> list[Evidence]:
        acl_scope = set(allowed_acl_ids)
        with self._lock:
            candidates = [
                item
                for item in self._units.values()
                if item.tenant_id == tenant_id
                and item.acl_id in acl_scope
                and item.unit.modality is route.modality
                and item.unit.embedding_model_id == route.embedding_model_id
                and item.unit.embedding_model_version
                == route.embedding_model_version
                and len(item.unit.dense_embedding) == len(route.dense_embedding)
            ]
        dense_rank = sorted(
            candidates,
            key=lambda item: (
                -_cosine(route.dense_embedding, item.unit.dense_embedding),
                item.unit.unit_id,
            ),
        )
        query_terms = _terms(route.query)
        lexical_rank = sorted(
            candidates,
            key=lambda item: (
                -_lexical_score(query_terms, item.unit.content),
                item.unit.unit_id,
            ),
        )
        scores: dict[str, float] = {}
        for ranking in (dense_rank, lexical_rank):
            for rank, item in enumerate(ranking, start=1):
                scores[item.unit.unit_id] = (
                    scores.get(item.unit.unit_id, 0.0) + 1.0 / (60 + rank)
                )
        ranked = sorted(
            candidates,
            key=lambda item: (-scores[item.unit.unit_id], item.unit.unit_id),
        )[: route.top_k]
        return [
            _to_evidence(item, route.route_id, scores[item.unit.unit_id])
            for item in ranked
        ]


def validate_index_command(command: IndexAssetCommand) -> None:
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
    if first.modality not in {Modality.DOCUMENT, Modality.IMAGE, Modality.VIDEO}:
        raise ValueError("index unit modality must be document, image, or video")
    if first.modality is Modality.IMAGE and (
        len(command.units) != 1 or command.append_to_asset_version
    ):
        raise ValueError("one image asset version requires one replacement unit")

    unit_ids: set[str] = set()
    ordinals: set[int] = set()
    for unit in command.units:
        _validate_unit(unit, first)
        if unit.unit_id in unit_ids or unit.ordinal in ordinals:
            raise ValueError("index unit IDs and ordinals must be unique")
        unit_ids.add(unit.unit_id)
        ordinals.add(unit.ordinal)


def _validate_unit(unit: IndexUnit, first: IndexUnit) -> None:
    try:
        content_size = len(unit.content.encode("utf-8"))
        title_size = len(unit.title.encode("utf-8"))
    except UnicodeEncodeError as error:
        raise ValueError("index unit text must be valid UTF-8") from error
    if (
        not unit.unit_id
        or len(unit.unit_id.encode("utf-8")) > 256
        or not 1 <= content_size <= 65_535
        or title_size > 2_048
        or unit.ordinal < 0
        or unit.page_number < 0
        or len(unit.content_sha256) != 64
        or not 1 <= len(unit.dense_embedding) <= 65_536
        or any(not isfinite(value) for value in unit.dense_embedding)
        or not unit.embedding_model_id
        or not unit.embedding_model_version
    ):
        raise ValueError("index unit content and embedding must be valid")
    if (
        unit.modality is not first.modality
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
        _validate_image(unit, metadata)
    elif unit.modality is Modality.VIDEO:
        _validate_video(unit, metadata)


def _validate_image(unit: IndexUnit, metadata: dict[str, str]) -> None:
    width = _unsigned(metadata, "width")
    height = _unsigned(metadata, "height")
    if (
        not unit.title
        or metadata.get("media_type") not in SUPPORTED_IMAGE_TYPES
        or not 1 <= width <= UINT32_MAX
        or not 1 <= height <= UINT32_MAX
        or not metadata.get("vision_model_id")
        or not metadata.get("vision_model_version")
    ):
        raise ValueError("image index metadata and caption must be valid")


def _validate_video(unit: IndexUnit, metadata: dict[str, str]) -> None:
    duration = _unsigned(metadata, "duration_ms")
    width = _unsigned(metadata, "width")
    height = _unsigned(metadata, "height")
    start = _unsigned(metadata, "start_ms")
    end = _unsigned(metadata, "end_ms")
    keyframe = _unsigned(metadata, "keyframe_ms")
    if (
        not unit.title
        or metadata.get("media_type") not in SUPPORTED_VIDEO_TYPES
        or not 1 <= duration <= 86_400_000
        or not 1 <= width <= 32_768
        or not 1 <= height <= 32_768
        or width * height > 268_435_456
        or not 0 <= start < end <= duration
        or not start <= keyframe < end
        or not metadata.get("caption")
        or not metadata.get("speech_model_id")
        or not metadata.get("speech_model_version")
        or not metadata.get("vision_model_id")
        or not metadata.get("vision_model_version")
    ):
        raise ValueError("video index metadata and segment bounds are invalid")


def _unsigned(metadata: dict[str, str], name: str) -> int:
    value = metadata.get(name, "")
    if not value.isascii() or not value.isdecimal():
        raise ValueError("timestamps and dimensions must be unsigned integers")
    return int(value)


def _to_evidence(item: IndexedUnit, route_id: str, score: float) -> Evidence:
    unit = item.unit
    stored = dict(unit.metadata)
    metadata = {
        "asset_id": item.asset_id,
        "asset_version_id": item.asset_version_id,
        "route_id": route_id,
    }
    title = unit.title
    if unit.modality is Modality.DOCUMENT:
        metadata.update(
            ordinal=str(unit.ordinal), page_number=str(unit.page_number)
        )
    elif unit.modality is Modality.IMAGE:
        metadata.update(
            (key, _truncate_utf8(stored.get(key, ""), 16_384))
            for key in ("media_type", "width", "height", "ocr_text")
        )
    else:
        title = stored.get("caption", unit.title)
        metadata.update(
            (key, _truncate_utf8(stored.get(key, ""), 16_384))
            for key in (
                "ordinal",
                "media_type",
                "duration_ms",
                "width",
                "height",
                "start_ms",
                "end_ms",
                "keyframe_ms",
                "ocr_text",
                "transcript",
            )
        )
        metadata["ordinal"] = str(unit.ordinal)
    return Evidence(
        evidence_id=unit.unit_id,
        content=unit.content,
        modality=unit.modality,
        source_scope=SourceScope.LOCAL,
        title=title,
        source=item.object_key,
        score=score,
        metadata=metadata,
        content_sha256=unit.content_sha256,
    )


def _collection_alias(unit: IndexUnit) -> str:
    value = f"{unit.embedding_model_id}:{unit.embedding_model_version}".encode()
    hash_value = 14_695_981_039_346_656_037
    for byte in value:
        hash_value = (
            (hash_value ^ byte) * 1_099_511_628_211
        ) & 0xFFFFFFFFFFFFFFFF
    return (
        f"rag_{unit.modality.name.lower()}_v1_"
        f"{hash_value:016x}_{len(unit.dense_embedding)}"
    )


def _cosine(left: tuple[float, ...], right: tuple[float, ...]) -> float:
    dot = sum(a * b for a, b in zip(left, right, strict=True))
    left_norm = sqrt(sum(value * value for value in left))
    right_norm = sqrt(sum(value * value for value in right))
    return dot / (left_norm * right_norm) if left_norm and right_norm else 0.0


def _terms(text: str) -> tuple[str, ...]:
    return tuple(re.findall(r"[a-z0-9]+", text.lower()))


def _lexical_score(query_terms: tuple[str, ...], content: str) -> float:
    if not query_terms:
        return 0.0
    content_terms = set(_terms(content))
    return sum(term in content_terms for term in query_terms) / len(query_terms)


def _truncate_utf8(value: str, max_bytes: int) -> str:
    encoded = value.encode("utf-8")
    if len(encoded) <= max_bytes:
        return value
    return encoded[:max_bytes].decode("utf-8", errors="ignore")
