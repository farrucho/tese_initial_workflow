#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
g++ -O2 -std=c++17 -Wall -Wextra -Wpedantic -Werror \
    "$HERE/capture_atca.cpp" -o "$HERE/capture_atca"
exec "$HERE/capture_atca" "$@"
