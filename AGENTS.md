# Nano AI Search repository guide

This file applies to the entire repository. Keep it concise and update it when
the architecture or the supported workflow changes.

## Product and architecture

Nano AI Search is a lightweight web-first AI search application:

- `services/web_ui/`: React/Vite user interface.
- `services/python_api/`: FastAPI orchestration, model adapters, web search,
  ingestion, and optional infrastructure integrations.
- `core/`: C++20 evidence processing and in-memory multimodal index.
- Python and C++ communicate only through HTTP/JSON. The endpoints are
  `GET /health`, `POST /v1/execute-plan`, and `POST /v1/index-asset`.
- The C++ target is built with Blade. Do not reintroduce gRPC, Protobuf, CMake,
  Conan, or the removed Milvus adapters unless the user explicitly asks for an
  architectural reversal.
- The default Nano path is web search. MySQL, Kafka, object storage, embeddings,
  vision, and speech services are optional capabilities, not startup
  prerequisites.
- The C++ index is process-local memory and is cleared on restart.

## Configuration

- Runtime configuration belongs in
  `services/python_api/src/rag_api/config/`.
- Import the stable public API with `from rag_api.config import Settings`; do
  not import configuration implementation modules from business packages.
- Environment variables use the `RAG_` prefix. Keep `.env.example` and
  `docs/configuration.md` synchronized with user-facing settings.
- Never commit `.env`, API keys, access tokens, credentials, or production
  endpoint secrets.

## Supported commands

Bootstrap a fresh checkout:

```bash
./scripts/bootstrap.sh
```

Build the C++ core:

```bash
./scripts/blade.sh build //core:nano_core
```

Start the full local stack after creating `.env`:

```bash
./scripts/run.sh
```

The default local ports are UI `5173`, Python API `8000`, and C++ Core `8081`.
Use `NANO_WEB_PORT`, `NANO_API_PORT`, and `NANO_CORE_PORT` when ports conflict.

## Required verification

The automated test modules were intentionally removed during the Nano
reduction. Do not recreate a broad test stack unless requested. For ordinary
changes, run the relevant build and smoke checks:

```bash
./scripts/blade.sh build //core:nano_core
PYTHONPATH=services/python_api/src .venv/bin/python -m compileall -q services/python_api/src
.venv/bin/pip check
npm run build --prefix services/web_ui
git diff --check
```

For runtime or transport changes, also start the stack and verify
`/health/ready`. Never use a real API key in committed fixtures or command
output.

## Change discipline

- Preserve the HTTP/JSON boundary and the public `rag_api.config` import path.
- Keep generated outputs out of Git: `.venv/`, `.tools/`, `node_modules/`,
  `dist/`, `build_release/`, and `blade-bin` are local artifacts.
- Prefer small, reversible changes and do not overwrite unrelated user work.
- Update `README.md`, `.env.example`, or `docs/configuration.md` when startup,
  dependencies, ports, or required configuration change.
