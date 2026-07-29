#!/bin/sh
# Compile and run the dwell state machine tests natively.
#
# tracker.c and zone.c depend only on glib, jansson and libm, so the timing
# rules can be exercised on the host instead of on a camera. Run from the
# repository root:
#
#     docker run --rm -v "$PWD":/src -w /src ubuntu:24.04 sh test/run.sh
#
set -e

if ! pkg-config --exists glib-2.0 jansson 2>/dev/null; then
    echo "installing build dependencies..."
    apt-get update -qq >/dev/null
    apt-get install -y -qq gcc pkg-config libglib2.0-dev libjansson-dev >/dev/null
fi

CFLAGS="$(pkg-config --cflags glib-2.0 jansson) -Wall -Wextra -std=c17 -g -O1"
LIBS="$(pkg-config --libs glib-2.0 jansson) -lm"

# shellcheck disable=SC2086
gcc test/test_tracker.c app/tracker.c app/zone.c $CFLAGS $LIBS -o /tmp/test_tracker

/tmp/test_tracker
