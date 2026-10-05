"""ASGI entry point for the pure-Python core service."""

from __future__ import annotations

from fastapi import FastAPI, HTTPException

from rag_api import __version__
from rag_api.core_client import (
    CoreHealth,
    CorePlanResult,
    IndexAssetCommand,
    IndexAssetResult,
)
from rag_api.domain import ExecutionPlan

from .engine import InMemoryCore


core = InMemoryCore()
app = FastAPI(title="Nano AI Search Core", version=__version__)


@app.get("/health", response_model=CoreHealth)
async def health() -> CoreHealth:
    return await core.health()


@app.post("/v1/execute-plan", response_model=CorePlanResult)
async def execute_plan(plan: ExecutionPlan) -> CorePlanResult:
    try:
        return await core.execute_plan(plan)
    except ValueError as error:
        raise HTTPException(status_code=400, detail=str(error)) from error


@app.post("/v1/index-asset", response_model=IndexAssetResult)
async def index_asset(command: IndexAssetCommand) -> IndexAssetResult:
    try:
        return await core.index_asset(command)
    except ValueError as error:
        raise HTTPException(status_code=400, detail=str(error)) from error
