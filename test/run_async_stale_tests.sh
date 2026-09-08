#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
make -j8 test/test_async_stale >/dev/null
./test/test_async_stale
async_test_dir=$(mktemp -d /tmp/stellar-async-stale-XXXXXX)
trap 'rm -rf "$async_test_dir"' EXIT
timeout 30 ./test/test_async_stale pipeline "$async_test_dir"
