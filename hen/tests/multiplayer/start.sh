#!/bin/sh
# start.sh A|B : starts one test instance with Mario Kart 7 from its own user folder, with
# scripted input and the command trace on. KAIZHAR_TEST_ROOT: the work folder; KAIZHAR_TEST_APP:
# the desktop test build's program (.../azahar.app/Contents/MacOS/azahar).
: "${KAIZHAR_TEST_ROOT:?set KAIZHAR_TEST_ROOT}" "${KAIZHAR_TEST_APP:?set KAIZHAR_TEST_APP}"
I=$1
GAME="$KAIZHAR_TEST_ROOT/$I/Library/Application Support/Azahar/sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title/00040000/00030800/content/00000000.app"
mkdir -p "$KAIZHAR_TEST_ROOT/$I/ctl"
HOME="$KAIZHAR_TEST_ROOT/$I" KAIZHAR_TEST_DIR="$KAIZHAR_TEST_ROOT/$I/ctl" KAIZHAR_TEST_TRACE=1 "$KAIZHAR_TEST_APP" "$GAME" > "$KAIZHAR_TEST_ROOT/$I/run.log" 2>&1 &
echo $! > "$KAIZHAR_TEST_ROOT/$I/pid"
