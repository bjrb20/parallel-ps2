#!/bin/sh
# The core's audio queue (libretro/audio_queue.c) on the real retro_spsc:
# the ring mirrored where the host allows it, and on Linux a second lane
# with retro_spsc built without mirroring. ASan+UBSan off Windows; MSYS2
# MINGW64 builds and runs the plain one. "bench" times both shapes.
# Runnable from ANY directory - paths resolve relative to this script.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"

case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) WINDOWS=1; EXE=.exe ;;
  *)                    WINDOWS=0; EXE=""   ;;
esac

FLAGS="-std=c89 -pedantic -Wno-long-long -Wall -Wextra -Werror=implicit-function-declaration"

build() { # output, extra flags, extra flags for retro_spsc.c
	OBJ="$DIR/.obj_$1"
	rm -rf "$OBJ" && mkdir -p "$OBJ"
	"$CC" -w $2 $3 -I "$LC/include" -c "$LC/queues/retro_spsc.c" -o "$OBJ/retro_spsc.o"
	"$CC" $FLAGS $2 -I "$ROOT/libretro" -I "$LC/include" \
		-o "$DIR/$1$EXE" "$DIR/main.c" "$ROOT/libretro/audio_queue.c" "$OBJ/retro_spsc.o"
	rm -rf "$OBJ"
}

# On Linux and Windows the ring is mirrored, and that is checked; on
# Linux the second lane is not.
PLAIN=0
ONE=""
case "$(uname -s 2>/dev/null)" in
  Linux)                PLAIN=1; ONE="-DEXPECT_ONE_BATCH" ;;
  MINGW*|MSYS*|CYGWIN*) ONE="-DEXPECT_ONE_BATCH" ;;
esac

build audioqueue_test "-O2 $ONE $SANFLAGS"
[ "$PLAIN" = "1" ] && build audioqueue_test_plain "-O2 $SANFLAGS" "-U__linux__"

if [ "$1" = "bench" ]; then
	"$DIR/audioqueue_test$EXE" bench
	[ "$PLAIN" = "1" ] && "$DIR/audioqueue_test_plain" bench
	exit 0
fi

echo "== mirrored where available =="
"$DIR/audioqueue_test$EXE"
if [ "$PLAIN" = "1" ]; then
	echo "== not mirrored =="
	"$DIR/audioqueue_test_plain"
fi
if [ "$WINDOWS" != "1" ]; then
	build audioqueue_test_asan "-O1 -g $ONE -fsanitize=address,undefined -fno-sanitize-recover=all"
	echo "== mirrored, ASan+UBSan =="
	"$DIR/audioqueue_test_asan"
fi
