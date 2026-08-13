/* gsusb_dump - candump(1)-style live CAN frame dump for gs_usb devices. */
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

static volatile sig_atomic_t g_stop;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static void print_frame(unsigned int channel, const gsusb_frame *f)
{
	uint32_t id = f->can_id & (f->can_id & GSUSB_EFF_FLAG ? GSUSB_EFF_MASK : GSUSB_SFF_MASK);
	int eff = (f->can_id & GSUSB_EFF_FLAG) != 0;
	int rtr = (f->can_id & GSUSB_RTR_FLAG) != 0;
	int err = (f->can_id & GSUSB_ERR_FLAG) != 0;

	if (f->timestamp_us || (f->flags & GSUSB_FRAME_FD))
		printf("(%10.6f) ", f->timestamp_us / 1e6);

	printf("can%u  %0*X  ", channel, eff ? 8 : 3, id);
	if (err)
		printf("ERR");
	else if (rtr)
		printf("R");

	if (f->flags & GSUSB_FRAME_FD)
		printf("#%c%c",
		       (f->flags & GSUSB_FRAME_BRS) ? 'B' : '-',
		       (f->flags & GSUSB_FRAME_ESI) ? 'E' : '-');
	else
		printf("[%u]", f->len);

	if (!rtr) {
		printf(" ");
		for (unsigned int i = 0; i < f->len; i++)
			printf("%02X ", f->data[i]);
	}
	printf("\n");
}

int main(int argc, char **argv)
{
	/* Force line buffering even when stdout isn't a TTY (piped to a file,
	 * or run under a wrapper like termux-usb that doesn't attach a real
	 * terminal) -- otherwise glibc/bionic fully-buffer stdout and frames
	 * only appear once the buffer fills or the process exits, defeating
	 * the point of a live dump. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	int vid = 0, pid = 0, bus = -1, addr = -1;
	unsigned int channel = 0;
	uint32_t bitrate = 500000, data_bitrate = 2000000;
	int fd_mode = 0, loopback = 0, listen_only = 0;

	static struct option opts[] = {
		{ "vid", required_argument, 0, 'v' },
		{ "pid", required_argument, 0, 'p' },
		{ "bus", required_argument, 0, 'b' },
		{ "addr", required_argument, 0, 'a' },
		{ "channel", required_argument, 0, 'c' },
		{ "bitrate", required_argument, 0, 'r' },
		{ "data-bitrate", required_argument, 0, 'd' },
		{ "fd", no_argument, 0, 'f' },
		{ "loopback", no_argument, 0, 'L' },
		{ "listen-only", no_argument, 0, 'l' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 },
	};

	int c;
	while ((c = getopt_long(argc, argv, "v:p:b:a:c:r:d:fLlh", opts, NULL)) != -1) {
		switch (c) {
		case 'v': vid = (int)strtoul(optarg, NULL, 16); break;
		case 'p': pid = (int)strtoul(optarg, NULL, 16); break;
		case 'b': bus = atoi(optarg); break;
		case 'a': addr = atoi(optarg); break;
		case 'c': channel = (unsigned int)atoi(optarg); break;
		case 'r': bitrate = (uint32_t)strtoul(optarg, NULL, 10); break;
		case 'd': data_bitrate = (uint32_t)strtoul(optarg, NULL, 10); break;
		case 'f': fd_mode = 1; break;
		case 'L': loopback = 1; break;
		case 'l': listen_only = 1; break;
		case 'h':
		default:
			fprintf(stderr,
				"usage: %s [--vid HEX --pid HEX] [--bus N --addr N] [--channel N]\n"
				"          [--bitrate BPS] [--fd [--data-bitrate BPS]] [--loopback] [--listen-only]\n",
				argv[0]);
			return c == 'h' ? 0 : 1;
		}
	}

	void *ctx = NULL;
	int rc = gsusb_init(&ctx);
	if (rc) {
		fprintf(stderr, "gsusb_dump: %s\n", gsusb_strerror(rc));
		return 1;
	}

	gsusb_dev *dev = gsusb_tool_open(ctx, vid, pid, bus, addr);
	gsusb_channel *ch = gsusb_channel_get(dev, channel);
	if (!ch) {
		fprintf(stderr, "gsusb_dump: channel %u does not exist on this device\n", channel);
		return 1;
	}
	if (fd_mode && !(gsusb_channel_features(ch) & GSUSB_FEATURE_FD)) {
		fprintf(stderr, "gsusb_dump: channel %u does not support CAN-FD\n", channel);
		return 1;
	}

	rc = gsusb_channel_set_bitrate(ch, bitrate, 0);
	if (rc) {
		fprintf(stderr, "gsusb_dump: set_bitrate(%u): %s\n", bitrate, gsusb_strerror(rc));
		return 1;
	}
	if (fd_mode) {
		rc = gsusb_channel_set_data_bitrate(ch, data_bitrate, 0);
		if (rc) {
			fprintf(stderr, "gsusb_dump: set_data_bitrate(%u): %s\n",
				data_bitrate, gsusb_strerror(rc));
			return 1;
		}
	}

	uint32_t mode = 0;
	if (fd_mode) mode |= GSUSB_MODE_FD;
	if (loopback) mode |= GSUSB_MODE_LOOPBACK;
	if (listen_only) mode |= GSUSB_MODE_LISTEN_ONLY;

	rc = gsusb_channel_start(ch, mode);
	if (rc) {
		fprintf(stderr, "gsusb_dump: start: %s\n", gsusb_strerror(rc));
		return 1;
	}

	if (fd_mode)
		fprintf(stderr, "gsusb_dump: listening on channel %u at %u bps (FD data phase %u bps), Ctrl-C to stop\n",
			channel, bitrate, data_bitrate);
	else
		fprintf(stderr, "gsusb_dump: listening on channel %u at %u bps, Ctrl-C to stop\n",
			channel, bitrate);

	signal(SIGINT, on_sigint);

	gsusb_frame f;
	while (!g_stop) {
		int r = gsusb_channel_recv(ch, &f, 500);
		if (r > 0)
			print_frame(channel, &f);
		else if (r < 0)
			break;
	}

	gsusb_channel_stop(ch);
	gsusb_close(dev);
	gsusb_exit(ctx);
	return 0;
}
