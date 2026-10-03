#!/usr/bin/env bash

set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
venv_dir="${repository_root}/.venv"
blade_dir="${repository_root}/.tools/blade-build"
blade_version="v3.1.0"

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

if ! command -v blade >/dev/null 2>&1 && [[ ! -x "${blade_dir}/blade" ]]; then
  mkdir -p "${repository_root}/.tools"
  git clone --depth 1 --branch "${blade_version}" \
    https://github.com/blade-build/blade-build.git "${blade_dir}"
fi

npm ci --prefix "${repository_root}/services/web_ui"
"${repository_root}/scripts/blade.sh" build //core:nano_core

echo "Bootstrap complete."
echo "Next: cp .env.example .env, fill RAG_CHAT_API_KEY, then ./scripts/run.sh"
