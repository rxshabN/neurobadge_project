#!/usr/bin/env bash
# Host-side logic verification for neurobadge.ino.
# Compiles the sketch against header stubs and exercises the decision logic
# with synthetic sensor series. No hardware required.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
cp "$here/../neurobadge/neurobadge.ino" "$here/stubs/sketch.cpp"
cp "$here/test.cpp" "$here/stubs/test.cpp"
echo "Arduino prototype-order lint:"
python3 "$here/lint_prototype_order.py" "$here/../neurobadge/neurobadge.ino"
cd "$here/stubs"
g++ -fsyntax-only -std=gnu++17 -I. -Wall -Wno-unused-parameter -Wno-unused-variable sketch.cpp
g++ -std=gnu++17 -I. -Wall -Wno-unused-parameter -Wno-unused-variable -o test test.cpp
./test
