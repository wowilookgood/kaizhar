#!/bin/sh
# stop.sh : ends the test instances (the processes start.sh started, by their recorded ids; the
# program takes no polite request to end while a title runs).
: "${KAIZHAR_TEST_ROOT:?set KAIZHAR_TEST_ROOT}"
for I in A B; do
    if [ -f "$KAIZHAR_TEST_ROOT/$I/pid" ]; then
        kill -9 "$(cat "$KAIZHAR_TEST_ROOT/$I/pid")" 2>/dev/null
    fi
done
exit 0
