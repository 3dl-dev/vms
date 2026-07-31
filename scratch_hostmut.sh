#!/bin/bash
# scratch driver (NOT a deliverable): host-side mutation -> ctest dcl suite.
set -u
NAME="$1"; shift
echo "########## HOST MUTATION $NAME ##########"
"$@" || { echo "$NAME: anchor did not match"; exit 2; }
if git diff --quiet; then echo "$NAME: NO CHANGE -- broken fixture"; git checkout -- .; exit 2; fi
cmake --build build -j8 > /tmp/hm-$NAME-build.log 2>&1 || { echo "$NAME: build failed"; tail -5 /tmp/hm-$NAME-build.log; git checkout -- .; exit 2; }
(cd build && ctest -R dcl-integration --output-on-failure 2>&1) | grep -E "^FAIL:|Expected|tests passed|tests failed"
git checkout -- .
