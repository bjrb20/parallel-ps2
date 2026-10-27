/* The core's audio queue (libretro/audio_queue.c), on the real retro_spsc.
 *
 *   - streamed through many laps of the ring with reserves of every size
 *     SPU2 asks for and commits short of them, every sample reaches the
 *     frontend once, in order, and no batch splits a stereo frame;
 *   - each upload hands over exactly what was committed before its mark;
 *   - a savestate taken mid-stream and loaded back resumes the stream.
 *
 * Built twice by build.sh: with the ring mirrored, where every mark's
 * samples go out in one batch, and (Linux) with retro_spsc built without
 * mirroring, where the end of the ring splits a batch and a reserve can
 * land in staging. With an argument it times both shapes instead.
 * C89. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <retro_spsc.h>

#include "audio_queue.h"

static int s_failures;

static void check(int ok, const char *what)
{
	if (!ok)
	{
		printf("  FAIL: %s\n", what);
		s_failures++;
	}
}

static unsigned s_seed = 2024;
static unsigned rnd(unsigned n)
{
	s_seed = s_seed * 1103515245u + 12345u;
	return (s_seed >> 8) % n;
}

/* The frontend: checks the stream and counts. */
static uint32_t s_expect;     /* next sample value the frontend should see */
static unsigned long s_frames, s_batches, s_uploads, s_bad;
static size_t s_largest;

static size_t frontend_batch(const int16_t *data, size_t frames)
{
	size_t i;
	for (i = 0; i < frames * 2; i++)
	{
		if ((uint16_t)data[i] != (uint16_t)s_expect)
			s_bad++;
		s_expect++;
	}
	s_frames += frames;
	s_batches++;
	if (frames > s_largest)
		s_largest = frames;
	return frames;
}

static size_t null_batch(const int16_t *data, size_t frames)
{
	(void)data;
	return frames;
}

/* The producer: SPU2's shape, a reserve of up to max samples filled a
 * pair at a time and committed short of it. */
static uint32_t s_next;       /* next sample value to produce */

static void produce(int32_t max, int32_t count)
{
	int16_t *dst = retro_audio_reserve(max);
	int32_t  i;
	if (!dst)
		return;
	for (i = 0; i < count; i++)
		dst[i] = (int16_t)(uint16_t)s_next++;
	retro_audio_commit(count);
}

static void run_stream(unsigned long marks)
{
	unsigned long m;
	for (m = 0; m < marks; m++)
	{
		/* A frame's worth, 1600 samples at 48 kHz/60 Hz, in slices. */
		int32_t left = 1400 + (int32_t)rnd(400) * 2;
		uint32_t mark;
		while (left > 0)
		{
			int32_t max   = 2 + (int32_t)rnd(4800) * 2;
			int32_t count = (int32_t)rnd((unsigned)(max / 2) + 1) * 2;
			if (count > left)
				count = left;
			produce(max, count);
			left -= count;
		}
		mark = retro_audio_mark();
		audio_queue_upload(mark, frontend_batch);
		s_uploads++;
		check(s_expect == s_next, "an upload did not hand over everything before its mark");
	}
}

static double now_sec(void)
{
	return (double)clock() / CLOCKS_PER_SEC;
}

static int bench(void)
{
	static int16_t pairs[2];
	unsigned long i, laps = 0;
	double t0, t;
	(void)pairs;
	if (!audio_queue_init())
		return 1;
	t0 = now_sec();
	/* 48 kHz in SPU2's slices, uploaded per frame: 800 frames a mark. */
	for (i = 0; i < 2000000ul; i++)
	{
		int16_t *dst = retro_audio_reserve(192);
		if (dst)
		{
			memset(dst, (int)(i & 0xff), 192 * sizeof(int16_t));
			retro_audio_commit(192);
		}
		if ((i & 7) == 7)
			audio_queue_upload(retro_audio_mark(), null_batch);
	}
	t = now_sec() - t0;
	laps = (unsigned long)((double)i * 192 * 2 / (1 << 18));
	printf("audio queue bench: %.3f s for %lu reserves (%lu laps of the ring)\n", t, i, laps);
	audio_queue_free();
	return 0;
}

int main(int argc, char **argv)
{
	static uint8_t state[1 << 18];
	uint32_t before;
	size_t saved;

	(void)argv;
	if (argc > 1)
		return bench();

	check(audio_queue_init(), "the queue does not allocate");

	/* Many laps: 256 KB is about 1.4 s, this streams about 50 s. */
	run_stream(3000);
	check(s_bad == 0, "a sample reached the frontend out of order or changed");
	check(s_frames * 2 == (unsigned long)s_next, "samples were lost or repeated");
	check(audio_queue_dropped == 0, "samples were dropped with the frontend keeping up");

	/* Savestate mid-stream: produce past a mark, save, load, resume. */
	produce(800, 800);
	before = retro_audio_mark();
	produce(600, 600);
	saved = audio_queue_save(state, sizeof(state), before, &before);
	check(saved == 1400 * sizeof(int16_t), "the savestate does not hold what was queued");
	check(before == 800 * sizeof(int16_t), "the savestate's mark is not where it was");
	audio_queue_discard();
	before = audio_queue_load(state, saved, before);
	audio_queue_upload(before, frontend_batch);
	check(s_expect == s_next - 600, "the loaded mark does not hand over what came before it");
	audio_queue_upload(retro_audio_mark(), frontend_batch);
	check(s_expect == s_next, "the rest of the loaded queue was not handed over");
	run_stream(50);
	check(s_bad == 0, "the stream was not intact after a load");

#ifdef EXPECT_ONE_BATCH
	/* Mirrored: the end of the ring never splits an upload. */
	check(s_batches == s_uploads + 2, "an upload took more than one batch on a mirrored ring");
#endif
	printf("  %lu frames in %lu batches for %lu uploads\n",
			s_frames, s_batches, s_uploads + 2);
	audio_queue_free();
	printf(s_failures ? "audio queue: FAILED (%d)\n" : "audio queue: ok\n", s_failures);
	return s_failures != 0;
}
