#!/bin/sh
# make_host_tree.sh WORKDIR : a source tree for a desktop build that carries the code under test.
#
# Kaizhar's own tree does not build for a computer (its JIT and renderer changes are the
# console's). The local wireless code is shared with upstream, though, so the test build is
# upstream at the commit Kaizhar is based on, with these files taken from this tree by link:
# the download play and local wireless services, the room client, the service dispatcher (for
# the command trace) and the pad input (for the scripted input). Edits here reach the next
# desktop build without another step.
set -e
WORK=${1:?work folder}
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
BASE=${KAIZHAR_UPSTREAM_BASE:-2126.2}
TREE="$WORK/hostsrc"
mkdir -p "$TREE" "$WORK/clean"
git -C "$REPO" archive "$BASE" | tar -x -C "$TREE"
# The three externals the console build patches, as committed (unpatched).
for e in dynarmic oaknut cryptopp; do
    mkdir -p "$WORK/clean/$e"
    git -C "$REPO/externals/$e" archive HEAD | tar -x -C "$WORK/clean/$e"
done
mkdir -p "$WORK/clean/dynarmic/externals/mcl.clean"
git -C "$REPO/externals/dynarmic/externals/mcl" archive HEAD | tar -x -C "$WORK/clean/dynarmic/externals/mcl.clean"
for e in "$REPO"/externals/dynarmic/externals/*/; do
    n=$(basename "$e")
    [ -n "$(ls -A "$WORK/clean/dynarmic/externals/$n" 2>/dev/null)" ] && continue
    rmdir "${WORK:?}/clean/dynarmic/externals/${n:?}" 2>/dev/null || true
    if [ "$n" = mcl ]; then mv "$WORK/clean/dynarmic/externals/mcl.clean" "$WORK/clean/dynarmic/externals/mcl"
    else ln -s "$e" "$WORK/clean/dynarmic/externals/$n"; fi
done
# Submodule folders (empty in the archive): the three above from the clean copies, the rest
# from this tree.
(cd "$TREE" && find . -type d -empty) | while read -r d; do
    d=${d#./}
    case "$d" in
        externals/dynarmic|externals/oaknut|externals/cryptopp) src="$WORK/clean/${d#externals/}" ;;
        *) src="$REPO/$d" ;;
    esac
    if [ -d "$src" ] && [ -n "$(ls -A "$src" 2>/dev/null)" ]; then
        rmdir "${TREE:?}/${d:?}" && ln -s "$src" "$TREE/$d"
    fi
done
# The code under test, by link.
for f in src/core/hle/service/dlp/dlp.cpp src/core/hle/service/dlp/dlp.h \
         src/core/hle/service/dlp/dlp_clt_base.cpp src/core/hle/service/dlp/dlp_clt_base.h \
         src/core/hle/service/dlp/dlp_srvr.cpp src/core/hle/service/dlp/dlp_srvr.h \
         src/core/hle/service/nwm/nwm_uds.cpp src/core/hle/service/hid/hid.cpp \
         src/core/hle/service/hid/hid_test_input.cpp src/core/hle/service/hid/hid_test_input.h \
         src/core/hle/service/service.cpp src/network/room_member.cpp; do
    ln -sf "$REPO/$f" "$TREE/$f"
done
# What upstream's own files need for them: the test input in the build, and the download play
# threads ended before the system shuts down (core.cpp here has console-only changes besides).
python3 - "$TREE" <<'PY'
import sys
tree = sys.argv[1]
def sub(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old)
    open(path, "w").write(s.replace(old, new))
sub(tree + "/src/core/CMakeLists.txt", "    hle/service/hid/hid.cpp\n    hle/service/hid/hid.h",
    "    hle/service/hid/hid.cpp\n    hle/service/hid/hid.h\n    hle/service/hid/hid_test_input.cpp\n    hle/service/hid/hid_test_input.h")
sub(tree + "/src/core/core.cpp", "    // Shutdown emulation session\n    is_powered_on = false;\n",
    "    if (service_manager) {\n        Service::DLP::StopWorkers(*this);\n    }\n\n    // Shutdown emulation session\n    is_powered_on = false;\n")
sub(tree + "/src/core/core.cpp", '#include "core/hle/service/sm/sm.h"',
    '#include "core/hle/service/dlp/dlp.h"\n#include "core/hle/service/sm/sm.h"')
PY
echo "source tree: $TREE"
echo "configure and build:"
echo "  cmake -S $TREE -B $WORK/build -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=ON -DENABLE_QT_TRANSLATION=OFF -DENABLE_SDL2=ON -DENABLE_TESTS=OFF -DENABLE_ROOM=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_SCRIPTING=OFF -DENABLE_CUBEB=OFF -DENABLE_OPENAL=OFF -DENABLE_LIBUSB=OFF -DENABLE_LTO=OFF -DCITRA_USE_PRECOMPILED_HEADERS=OFF -DCITRA_WARNINGS_AS_ERRORS=OFF -DENABLE_VULKAN=ON -DENABLE_GDBSTUB=OFF '-DCMAKE_EXE_LINKER_FLAGS=-framework QuartzCore'"
echo "  cmake --build $WORK/build -- -j14"
