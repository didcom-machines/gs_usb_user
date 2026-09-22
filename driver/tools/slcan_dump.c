/* slcan_dump - candump(1)-style live CAN frame dump for SLCAN (serial-line
 * CAN) adapters -- e.g. CANable2 boards running SLCAN firmware, exposed by
 * the kernel's generic cdc_acm driver as /dev/ttyACM*. See
 * driver/include/slcan.h; this is a separate backend from gsusb_dump,
 * which is for gs_usb-protocol devices instead.
 */
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/slcan.h"

static volatile sig_atomic_t g_stop;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static void print_frame(const slcan_frame *f)
{
	int eff = (f->can_id & SLCAN_EFF_FLAG) != 0;
	int rtr = (f->can_id & SLCAN_RTR_FLAG) != 0;
	uint32_t id = f->can_id & (eff ? SLCAN_EFF_MASK : SLCAN_SFF_MASK);

	printf("slcan0  %0*X  ", eff ? 8 : 3, id);
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
	setvbuf(stdout, NULL, _IOLBF, 0); /* line-buffer even when not a TTY */

	const char *device = NULL;
	uint32_t bitrate = 500000;
	int listen_only = 0;

	static struct option opts[] = {
		{ "device", required_argument, 0, 'D' },
		{ "bitrate", required_argument, 0, 'r' },
		{ "listen-only", no_argument, 0, 'l' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 },
	};

	int c;
	while ((c = getopt_long(argc, argv, "D:r:lh", opts, NULL)) != -1) {
		switch (c) {
		case 'D': device = optarg; break;
		case 'r': bitrate = (uint32_t)strtoul(optarg, NULL, 10); break;
		case 'l': listen_only = 1; break;
		case 'h':
		default:
			fprintf(stderr,
				"usage: %s --device /dev/ttyACM0 [--bitrate BPS] [--listen-only]\n"
				"  --device PATH   serial device the adapter is on (required)\n"
				"  --bitrate BPS   one of 10000 20000 50000 100000 125000 250000\n"
				"                  500000 800000 1000000 (default 500000)\n"
				"  --listen-only   never ACK or transmit onto the bus\n",
				argv[0]);
			return c == 'h' ? 0 : 1;
		}
	}

	if (!device) {
		fprintf(stderr, "slcan_dump: --device is required (e.g. --device /dev/ttyACM0)\n");
		return 1;
	}

	char err[256];
	slcan_dev *dev = slcan_open(device, err, sizeof(err));
	if (!dev) {
		fprintf(stderr, "slcan_dump: %s\n", err);
		return 1;
	}

	int rc = slcan_set_bitrate(dev, bitrate);
	if (rc) {
		fprintf(stderr, "slcan_dump: set_bitrate(%u): %s\n", bitrate, slcan_strerror(rc));
		slcan_close(dev);
		return 1;
	}

	rc = slcan_start(dev, listen_only);
	if (rc) {
		fprintf(stderr, "slcan_dump: start: %s\n", slcan_strerror(rc));
		slcan_close(dev);
		return 1;
	}

	fprintf(stderr, "slcan_dump: listening on %s at %u bps, Ctrl-C to stop\n", device, bitrate);
	signal(SIGINT, on_sigint);

	slcan_frame f;
	while (!g_stop) {
		int r = slcan_recv(dev, &f, 500);
		if (r > 0)
			print_frame(&f);
		else if (r < 0)
			break;
	}

	slcan_stop(dev);
	slcan_close(dev);
	return 0;
}
