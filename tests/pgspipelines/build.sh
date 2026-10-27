#!/bin/sh
# paraLLEl-GS builds its compute pipelines ahead on threads of its own,
# the ones the last session used first. Runs the built core through
# tests/vknegotiate (lavapipe will do) on tests/swdraw's points case.
#
# A first run starts with an empty cache directory: it builds what it
# draws with on the frame's threads and writes the list of those
# pipelines. The driver's cache file is then deleted, as a driver update
# makes it useless, and the next run must build none on the frame's
# threads: the list and the compile threads give it every one, and the
# GS memory the case leaves is the same. A cache another driver made
# (its UUID changed) is dropped and the list still gives them all.
#
# Then the list is damaged four ways (cut short, a byte changed, another
# version, not a list at all): each is ignored with a warning, the run
# builds as many as a first run on the frame's threads and still holds
# the right memory, and the list it leaves is one the next run uses.
#
# Counting by thread needs thread names (pthread_getname_np); elsewhere
# the runs are made and checked, the counts are not.
#
# Runs the built core: pcsx2_libretro.so/.dll/.dylib at the top of the
# tree, or LRPS2_CORE. Without one, or without a Vulkan device, it is
# compiled and the run is skipped.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

COUNT=1
case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*) CORE="$ROOT/pcsx2_libretro.dll";   LIBS="";  COUNT=0 ;;
	Darwin)               CORE="$ROOT/pcsx2_libretro.dylib"; LIBS="";  COUNT=0 ;;
	*)                    CORE="$ROOT/pcsx2_libretro.so";    LIBS="-ldl -lpthread" ;;
esac
CORE=${LRPS2_CORE:-$CORE}

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
${CC:-cc} -std=c99 -pedantic -Wall -O2 ${CFLAGS} \
	-I "$ROOT/libretro/libretro-common/include" \
	-isystem "$ROOT/3rdparty/vulkan-headers/include" \
	-o "$SCRATCH/vknegotiate" "$ROOT/tests/vknegotiate/main.c" $LIBS
${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O2 \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$SCRATCH/swdraw" "$ROOT/tests/swdraw/main.c" $LIBS
"$SCRATCH/swdraw" --bios "$SCRATCH/points.bin" points > /dev/null

if [ ! -f "$CORE" ]; then
	echo "pgspipelines: skip: no core at $CORE (build it, or set LRPS2_CORE)"
	exit 0
fi

DATA="$SCRATCH/data"
CACHE="$DATA/pcsx2/cache"
LIST="$CACHE/pgs_pipeline_list.bin"
BLOB="$CACHE/pgs_pipeline_cache.bin"
mkdir -p "$DATA"
fail=0

# $1 name: one run on the points case; its log is $SCRATCH/$1.log
run()
{
	if ! VN_COUNT_PIPELINES=1 VN_LOAD_PAUSE_MS=${PAUSE_MS:-1500} VN_FRAMES=60 \
			VN_EXPECT_RUN=78563412 VN_IDLE_BIOS=1 VN_VERBOSE=1 \
			LRPS2_BIOS="$SCRATCH/points.bin" VN_RENDERER=paraLLEl-GS \
			timeout 600 "$SCRATCH/vknegotiate" "$CORE" "$DATA" v2 > "$SCRATCH/$1.log" 2>&1; then
		tail -n 30 "$SCRATCH/$1.log"
		echo "pgspipelines: FAIL: $1: the run failed"
		exit 1
	fi
	if grep -q "skip: no Vulkan device" "$SCRATCH/$1.log"; then
		echo "pgspipelines: skip: no Vulkan device"
		exit 0
	fi
	grep -q "holds 64 words of 78563412" "$SCRATCH/$1.log" \
		|| { echo "pgspipelines: FAIL: $1: the GS memory is not the case's"; fail=1; }
}
# pipelines the run built on the frame's threads
built() { sed -n "s/.*pipelines: \([0-9]*\) built on the frame's threads.*/\1/p" "$SCRATCH/$1.log"; }

# $1 name, $2 the count wanted (or "cold": more than none)
expect()
{
	[ "$COUNT" = 1 ] || return 0
	n=$(built "$1")
	if [ "$2" = cold ]; then
		[ -n "$n" ] && [ "$n" -gt 0 ] || { echo "pgspipelines: FAIL: $1: built ${n:-?} on the frame's threads, wanted some"; fail=1; }
	elif [ "$n" != "$2" ]; then
		echo "pgspipelines: FAIL: $1: built ${n:-?} on the frame's threads, wanted $2"
		fail=1
	fi
}

run cold
expect cold cold
COLD=$(built cold)
[ -s "$LIST" ] || { echo "pgspipelines: FAIL: no pipeline list written"; exit 1; }
cp "$LIST" "$SCRATCH/list.good"
echo "  cold: ${COLD:-?} built on the frame's threads, list written"

rm -f "$BLOB"
run warm
expect warm 0
echo "  driver cache deleted: $(built warm) on the frame's threads"

run warm_blob
expect warm_blob 0
echo "  driver cache kept: $(built warm_blob) on the frame's threads"

# another driver's cache: its UUID is the file's first bytes
if [ -s "$BLOB" ]; then
	printf 'XXXXXXXXXXXXXXXX' | dd of="$BLOB" bs=1 seek=0 count=16 conv=notrunc 2> /dev/null
	run foreign
	grep -q "Pipeline cache UUID changed" "$SCRATCH/foreign.log" \
		|| { echo "pgspipelines: FAIL: a cache of another driver was taken"; fail=1; }
	expect foreign 0
	echo "  another driver's cache: dropped, $(built foreign) on the frame's threads"
fi

for damage in short byte version junk; do
	cp "$SCRATCH/list.good" "$LIST"
	size=$(wc -c < "$LIST")
	case $damage in
		short)   head -c $((size - 5)) "$SCRATCH/list.good" > "$LIST" ;;
		byte)    printf '\377' | dd of="$LIST" bs=1 seek=$((size - 3)) count=1 conv=notrunc 2> /dev/null ;;
		version) printf '\011' | dd of="$LIST" bs=1 seek=4 count=1 conv=notrunc 2> /dev/null ;;
		junk)    head -c "$size" /dev/urandom > "$LIST" ;;
	esac
	rm -f "$BLOB"
	run "damaged_$damage"
	grep -q "Ignoring a damaged pipeline list" "$SCRATCH/damaged_$damage.log" \
		|| { echo "pgspipelines: FAIL: a list $damage was taken"; fail=1; }
	expect "damaged_$damage" "${COLD:-cold}"
	rm -f "$BLOB"
	run "after_$damage"
	expect "after_$damage" 0
	echo "  list damaged ($damage): ignored, replaced, used"
done

[ "$fail" = 0 ] || exit 1
echo "pgspipelines: ok"
