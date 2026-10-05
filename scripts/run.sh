#!/usr/bin/env bash

set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repository_root}"

if [[ ! -x .venv/bin/python ]]; then
  echo "Dependencies are missing. Run ./scripts/bootstrap.sh first." >&2
  exit 1
fi
if [[ ! -f .env ]]; then
  echo "Missing .env. Copy .env.example to .env and fill RAG_CHAT_API_KEY." >&2
  exit 1
fi
if ! grep -Eq '^RAG_CHAT_API_KEY=.+$' .env; then
  echo "RAG_CHAT_API_KEY is empty in .env." >&2
  exit 1
fi

pids=()
cleanup() {
  trap - EXIT INT TERM
  local pid
  for pid in "${pids[@]:-}"; do
    kill "${pid}" 2>/dev/null || true
  done
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

core_host="${NANO_CORE_HOST:-127.0.0.1}"
core_port="${NANO_CORE_PORT:-8081}"
api_host="${NANO_API_HOST:-127.0.0.1}"
api_port="${NANO_API_PORT:-8000}"
web_host="${NANO_WEB_HOST:-127.0.0.1}"
web_port="${NANO_WEB_PORT:-5173}"

export RAG_CORE_HTTP_BASE_URL="http://${core_host}:${core_port}"
export VITE_API_PROXY_TARGET="http://${api_host}:${api_port}"

PYTHONPATH=services/python_api/src \
  .venv/bin/python -m uvicorn rag_api.core.main:app \
  --host "${core_host}" --port "${core_port}" &
pids+=("$!")

PYTHONPATH=services/python_api/src \
  .venv/bin/python -m uvicorn rag_api.main:app \
  --host "${api_host}" --port "${api_port}" &
pids+=("$!")

npm run dev --prefix services/web_ui -- \
  --host "${web_host}" --port "${web_port}" &
pids+=("$!")

echo "Nano AI Search: http://${web_host}:${web_port}"
echo "API readiness:  http://${api_host}:${api_port}/health/ready"

set +e
wait -n "${pids[@]}"
status=$?
set -e
exit "${status}"
