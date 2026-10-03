#!/usr/bin/env bash

set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
local_blade="${repository_root}/.tools/blade-build/blade"
export PATH="${repository_root}/.venv/bin:${PATH}"

if command -v blade >/dev/null 2>&1; then
  exec blade "$@"
fi
if [[ -x "${local_blade}" ]]; then
  exec "${local_blade}" "$@"
fi

echo "Blade is not installed. Run ./scripts/bootstrap.sh first." >&2
exit 1
