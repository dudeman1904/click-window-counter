#!/bin/bash
# Host-side logic tests (no Geometry Dash needed). Needs g++ 13+ and clones of:
#   geode-sdk/result (v1.4.1), geode-sdk/json (v3.3.1), maxnut/GDReplayFormat
# Usage: DEPS=/path/to/dir/containing/those/clones ./tests/run.sh
set -e
DEPS=${DEPS:-..}
cd "$(dirname "$0")"
g++ -std=c++23 -I$DEPS/result/include -I$DEPS/matjson/include -I$DEPS/GDReplayFormat/include \
    io_test.cpp $DEPS/matjson/src/*.cpp -o /tmp/cw_io && /tmp/cw_io
g++ -std=c++23 search_test.cpp -o /tmp/cw_search && /tmp/cw_search
