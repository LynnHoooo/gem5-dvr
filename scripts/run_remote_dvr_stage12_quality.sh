#!/usr/bin/env bash
set -euo pipefail

source_root=${DVR_SOURCE_ROOT:-/home/lynnhoo/dvr-repro/source/gem5-runahead-dev-pre}
compiler=${CXX:-c++}
quality_output=${TMPDIR:-/tmp}/dvr_quality_smoke
predicate_output=${TMPDIR:-/tmp}/dvr_predicate_smoke
cache_event_output=${TMPDIR:-/tmp}/dvr_cache_quality_event_smoke

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I "${source_root}/src" \
  "${source_root}/src/cpu/o3/dvr_quality.cc" \
  "${source_root}/src/cpu/o3/dvr_quality_smoke.cc" \
  -o "${quality_output}"
"${quality_output}"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I "${source_root}/src" \
  "${source_root}/src/mem/cache/dvr_quality_event_smoke.cc" \
  -o "${cache_event_output}"
"${cache_event_output}"

"${compiler}" -std=c++17 -Wall -Wextra -Werror \
  -I "${source_root}/src" \
  "${source_root}/src/cpu/o3/dvr_predicate.cc" \
  "${source_root}/src/cpu/o3/dvr_predicate_smoke.cc" \
  -o "${predicate_output}"
"${predicate_output}"
printf 'DVR_PREDICATE_SMOKE_PASSED\n'
