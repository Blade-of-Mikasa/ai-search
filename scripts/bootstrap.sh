#!/usr/bin/env bash

set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
venv_dir="${repository_root}/.venv"
select_python() {
  if [[ -n "${RAG_PYTHON:-}" ]]; then
    printf '%s\n' "${RAG_PYTHON}"
    return
  fi
  local candidate
  for candidate in python3.14 python3.13 python3.12 python3.11; do
    if command -v "${candidate}" >/dev/null 2>&1; then
      command -v "${candidate}"
      return
    fi
  done
  echo "Python 3.11+ is required." >&2
  return 1
}

python_bin="$(select_python)"
"${python_bin}" -c 'import sys; assert sys.version_info >= (3, 11), sys.version'

if [[ ! -x "${venv_dir}/bin/python" ]]; then
  "${python_bin}" -m venv "${venv_dir}"
fi
"${venv_dir}/bin/python" -m pip install \
  --requirement "${repository_root}/requirements/api.lock"
"${venv_dir}/bin/python" -m pip install \
  --requirement "${repository_root}/requirements/build.lock"
"${venv_dir}/bin/python" -m pip install \
  --no-build-isolation --no-deps --editable "${repository_root}"

npm ci --prefix "${repository_root}/services/web_ui"

echo "Bootstrap complete."
echo "Next: cp .env.example .env, fill RAG_CHAT_API_KEY, then ./scripts/run.sh"
