# Local multiplayer test (two instances on a computer)

Two instances of a desktop build that carries Kaizhar's local wireless code meet in a
multiplayer room and play Mario Kart 7's local multiplayer against each other, driven by
script: create a room, join it, start, disband, and again. It needs no console and nobody at
the controls. It exists because of the fault described at the end; run it after any change to
`src/core/hle/service/dlp`, `src/core/hle/service/nwm` or `src/network`.

## What is here

- `make_host_tree.sh`: makes the source tree for the desktop test build (upstream at Kaizhar's
  base commit, the code under test linked in from this tree) and prints how to build it.
- `start.sh`, `stop.sh`: one instance with its own user folder; all of them ended.
- `ctl.py`: sends an instance commands (button presses, pictures of its screens, room joins).
- `repro.py`: the test. `repro.py 3` plays three sessions; `repro.py 3 swap` changes who hosts
  every session. Exit status 0 and `PASS` when every session's host was listed and joined.
- `make_mii_db.py`: puts one Mii into a test user folder (the game refuses to start without).
- `armdis.py`: disassembles the game's code as the trace dumps it (see below).

The emulator's side of it is in `src/core/hle/service/hid/hid_test_input.{h,cpp}` (the commands, the code dump)
and behind `KAIZHAR_TEST_*` variables in `service.cpp` and `dlp_clt_base.cpp`. None of it is in
the console's build.

## Setting up (macOS; once)

    export KAIZHAR_TEST_ROOT=/some/work/folder
    hen/tests/multiplayer/make_host_tree.sh $KAIZHAR_TEST_ROOT      # then the two cmake lines it prints
    export KAIZHAR_TEST_APP=$KAIZHAR_TEST_ROOT/build/bin/Release/azahar.app/Contents/MacOS/azahar
    for I in A B; do
        HOME=$KAIZHAR_TEST_ROOT/$I $KAIZHAR_TEST_APP -i "/path/to/Mario Kart 7 (decrypted).cia"
        sh hen/tests/multiplayer/start.sh $I; sleep 15; sh hen/tests/multiplayer/stop.sh   # the game makes its Mii database
        python3 hen/tests/multiplayer/make_mii_db.py $KAIZHAR_TEST_ROOT/$I Player$I
    done

The game file stays in the two test user folders; nothing else is copied.

## Running

    python3 hen/tests/multiplayer/repro.py 3 swap        # about five minutes

Two emulator windows open for the time of the test. Pictures of both instances' screens at each
step land in `$KAIZHAR_TEST_ROOT/shots`; each instance's emulator log (with every download play
and local wireless command, `TRACE` lines) is in its user folder under
`Library/Application Support/Azahar/log`.

`KAIZHAR_TEST_OLD_SERVER_TICK=1 python3 repro.py 3` is the control: it puts the old fault back
and must end in `FAIL` at the second session.

By hand: `ctl.py A press DOWN 120`, `ctl.py A shot name`, `ctl.py AB pair name`,
`ctl.py A status`. With `KAIZHAR_TEST_TRACE_CODE=/folder` set, an instance writes the game's
code to `/folder/code.bin` the first time the game discards a host, and logs the registers and
stack at that call; `armdis.py code.bin START END` reads it.

## The fault this was built for

After about 101 seconds of play, no local game could be joined any more: the joining console
found the host, read its title and server information, and the game threw it away at once,
every second, until the emulated console was restarted. The server information ends in the
system tick at which the server was last heard, and the game drops a server that has been
silent for five seconds. The emulator filled those eight bytes with a constant, which reads as
96 seconds after the start. Fixed in `dlp_clt_base.cpp` (`last_heard_tick`).

Found on the way, fixed in the same file: a host that was asked for its announcement before it
had sent it (the first second after it opens its game, or a slow moment) was ignored for the
rest of the search, and networks that are not download play networks were asked too.
