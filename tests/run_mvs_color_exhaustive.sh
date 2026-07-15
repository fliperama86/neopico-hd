#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/neopico-hd-host-tests"

mkdir -p "${build_dir}"

for model in 1 2; do
    binary="${build_dir}/mvs_color_exhaustive_model_${model}"
    "${CC:-cc}" \
        -std=c11 \
        -O2 \
        -Wall \
        -Wextra \
        -Werror \
        -DMVS_EFFECT_MODEL="${model}" \
        -I"${repo_root}/src/video" \
        "${repo_root}/tests/mvs_color_exhaustive.c" \
        -o "${binary}"
    "${binary}"
done

python3 "${repo_root}/tests/check_color_menu_contract.py"
