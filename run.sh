#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
g++ -O2 -std=c++17 -Wall -Wextra -Wpedantic -Werror \
    "$HERE/code/capture_atca.cpp" -o "$HERE/code/capture_atca"
exec "$HERE/code/capture_atca" "$@"
