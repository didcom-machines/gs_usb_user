/* gsusb_react - reference implementation of a listen-and-react loop:
 * watch the bus for a "key" frame and transmit a response immediately,
 * all from a single process/channel. This is the pattern for "listen and
 * write at the same time" -- see the two TODO blocks below for the two
 * places you customize: the match condition and the response to send.
 *
 * Architecture note (why this needs no extra threads/locking of your own):
 * gsusb_channel_start() already runs a background reader thread that fills
 * a per-channel queue; gsusb_channel_recv() just drains it and wakes up as
 * soon as a frame arrives, regardless of the timeout you pass. send() is an
 * independent bulk OUT transfer on the other USB endpoint, so it never
 * waits on, or blocks, the receive path. One recv()/send() loop in one
 * thread is the whole pattern.
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>

#include "common.h"

static volatile sig_atomic_t g_stop;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv)
{
	int vid = 0, pid = 0, bus = -1, addr = -1;
	unsigned int channel = 0;
	uint32_t bitrate = 500000;
	int loopback = 0;
	uint32_t key_id = 0x100, resp_id = 0x200;

	static struct option opts[] = {
		{ "vid", required_argument, 0, 'v' },
		{ "pid", required_argument, 0, 'p' },
		{ "bus", required_argument, 0, 'b' },
		{ "addr", required_argument, 0, 'a' },
		{ "channel", required_argument, 0, 'c' },
		{ "bitrate", required_argument, 0, 'r' },
		{ "key-id", required_argument, 0, 'k' },
		{ "resp-id", required_argument, 0, 'R' },
		{ "loopback", no_argument, 0, 'L' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 },
	};

	int c;
	while ((c = getopt_long(argc, argv, "v:p:b:a:c:r:k:R:Lh", opts, NULL)) != -1) {
		switch (c) {
		case 'v': vid = (int)strtoul(optarg, NULL, 16); break;
		case 'p': pid = (int)strtoul(optarg, NULL, 16); break;
		case 'b': bus = atoi(optarg); break;
		case 'a': addr = atoi(optarg); break;
		case 'c': channel = (unsigned int)atoi(optarg); break;
		case 'r': bitrate = (uint32_t)strtoul(optarg, NULL, 10); break;
		case 'k': key_id = (uint32_t)strtoul(optarg, NULL, 16); break;
		case 'R': resp_id = (uint32_t)strtoul(optarg, NULL, 16); break;
		case 'L': loopback = 1; break;
		case 'h':
		default:
			fprintf(stderr,
				"usage: %s [opts] [--key-id HEX] [--resp-id HEX] [--loopback]\n"
				"Watches for CAN id --key-id (default 0x100) and immediately\n"
				"transmits a response on --resp-id (default 0x200).\n"
				"Edit the two TODO blocks in tools/gsusb_react.c to customize\n"
				"the match condition and the response frame.\n",
				argv[0]);
			return c == 'h' ? 0 : 1;
		}
	}

	void *ctx = NULL;
	int rc = gsusb_init(&ctx);
	if (rc) { fprintf(stderr, "gsusb_react: %s\n", gsusb_strerror(rc)); return 1; }

	gsusb_dev *dev = gsusb_tool_open(ctx, vid, pid, bus, addr);
	gsusb_channel *ch = gsusb_channel_get(dev, channel);
	if (!ch) { fprintf(stderr, "gsusb_react: channel %u does not exist\n", channel); return 1; }

	rc = gsusb_channel_set_bitrate(ch, bitrate, 0);
	if (rc) { fprintf(stderr, "gsusb_react: set_bitrate: %s\n", gsusb_strerror(rc)); return 1; }

	rc = gsusb_channel_start(ch, loopback ? GSUSB_MODE_LOOPBACK : 0);
	if (rc) { fprintf(stderr, "gsusb_react: start: %s\n", gsusb_strerror(rc)); return 1; }

	fprintf(stderr, "gsusb_react: watching for id 0x%03X, reacting on 0x%03X, Ctrl-C to stop\n",
		key_id, resp_id);
	signal(SIGINT, on_sigint);

	gsusb_frame rx;
	unsigned long seen = 0, reacted = 0;

	while (!g_stop) {
		/* 200ms is just how often we re-check g_stop when the bus is
		 * idle -- it adds no latency to reacting on a real frame,
		 * since recv() wakes up the instant one arrives. */
		int r = gsusb_channel_recv(ch, &rx, 200);
		if (r == 0)
			continue;   /* idle timeout, loop to check g_stop */
		if (r < 0)
			break;      /* channel stopped/errored */

		/* --- TODO: customize the match condition --- */
		if (rx.can_id != key_id)
			continue;
		seen++;

		uint64_t t0 = now_ns();

		/* --- TODO: customize the response frame --- */
		gsusb_frame tx = { .can_id = resp_id, .len = 1, .data = { 0x01 } };

		rc = gsusb_channel_send(ch, &tx, 100);

		uint64_t t1 = now_ns();

		if (rc == 0) {
			reacted++;
			printf("key frame id=0x%03X -> reacted id=0x%03X in %llu us "
			       "(seen=%lu reacted=%lu)\n",
			       rx.can_id, tx.can_id,
			       (unsigned long long)((t1 - t0) / 1000), seen, reacted);
		} else {
			fprintf(stderr, "key frame id=0x%03X detected but send failed: %s\n",
				rx.can_id, gsusb_strerror(rc));
		}
	}

	gsusb_channel_stop(ch);
	gsusb_close(dev);
	gsusb_exit(ctx);
	return 0;
}
