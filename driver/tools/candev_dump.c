/* candev_dump - candump(1)-style live CAN frame dump using candev.h's
 * transparent auto-detection: tries gs_usb first, falls back to SLCAN. See
 * driver/include/candev.h.
 */
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/candev.h"

static volatile sig_atomic_t g_stop;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static void print_frame(const candev_frame *f)
{
	int eff = (f->can_id & CANDEV_EFF_FLAG) != 0;
	int rtr = (f->can_id & CANDEV_RTR_FLAG) != 0;
	uint32_t id = f->can_id & (eff ? CANDEV_EFF_MASK : CANDEV_SFF_MASK);

	printf("can0  %0*X  ", eff ? 8 : 3, id);
	if (rtr)
		printf("R");
	printf("[%u]", f->len);

	if (!rtr) {
		printf(" ");
		for (uint8_t i = 0; i < f->len; i++)
			printf("%02X ", f->data[i]);
	}
	printf("\n");
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	const char *slcan_hint = NULL;
	uint32_t bitrate = 500000;
	int listen_only = 0;

	static struct option opts[] = {
		{ "slcan-device", required_argument, 0, 'D' },
		{ "bitrate", required_argument, 0, 'r' },
		{ "listen-only", no_argument, 0, 'l' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 },
	};

	int c;
	while ((c = getopt_long(argc, argv, "D:r:lh", opts, NULL)) != -1) {
		switch (c) {
		case 'D': slcan_hint = optarg; break;
		case 'r': bitrate = (uint32_t)strtoul(optarg, NULL, 10); break;
		case 'l': listen_only = 1; break;
		case 'h':
		default:
			fprintf(stderr,
				"usage: %s [--slcan-device /dev/ttyACM0] [--bitrate BPS] [--listen-only]\n"
				"  Tries a gs_usb-protocol adapter first (by known VID:PID), then falls\n"
				"  back to SLCAN (scanning /dev/ttyACM*, or --slcan-device if given).\n"
				"  --bitrate BPS   one of 10000 20000 50000 100000 125000 250000\n"
				"                  500000 800000 1000000 (default 500000)\n"
				"  --listen-only   never ACK or transmit onto the bus\n",
				argv[0]);
			return c == 'h' ? 0 : 1;
		}
	}

	char err[256];
	candev *dev = candev_open(slcan_hint, err, sizeof(err));
	if (!dev) {
		fprintf(stderr, "candev_dump: %s\n", err);
		return 1;
	}

	int rc = candev_set_bitrate(dev, bitrate);
	if (rc) {
		fprintf(stderr, "candev_dump: set_bitrate(%u): %s\n", bitrate, candev_strerror(rc));
		candev_close(dev);
		return 1;
	}

	rc = candev_start(dev, listen_only);
	if (rc) {
		fprintf(stderr, "candev_dump: start: %s\n", candev_strerror(rc));
		candev_close(dev);
		return 1;
	}

	fprintf(stderr, "candev_dump: using %s backend at %u bps, Ctrl-C to stop\n",
		candev_backend_name(candev_which_backend(dev)), bitrate);
	signal(SIGINT, on_sigint);

	candev_frame f;
	while (!g_stop) {
		int r = candev_recv(dev, &f, 500);
		if (r > 0)
			print_frame(&f);
		else if (r < 0)
			break;
	}

	candev_stop(dev);
	candev_close(dev);
	return 0;
}
