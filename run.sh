#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
g++ -O2 -std=c++17 -Wall -Wextra -Wpedantic "$HERE/view_atca.cpp" -o "$HERE/view_atca"
exec "$HERE/view_atca" "$@"
