/* candev_send - cansend(1)-style frame transmit using candev.h's transparent
 * auto-detection: tries gs_usb first, falls back to SLCAN. Frame syntax:
 * <id>#{R|<hex bytes>}, e.g. "123#DEADBEEF" or "123#R" (same convention as
 * gsusb_send/slcan_send, minus the CAN-FD "##" extension -- candev is
 * classic-CAN only, see candev.h).
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/candev.h"

static int parse_frame(const char *s, candev_frame *f)
{
	memset(f, 0, sizeof(*f));

	const char *hash = strchr(s, '#');
	if (!hash || hash == s)
		return -1;

	size_t idlen = (size_t)(hash - s);
	if (idlen > 8)
		return -1;
	char idbuf[9];
	memcpy(idbuf, s, idlen);
	idbuf[idlen] = '\0';

	char *end = NULL;
	unsigned long id = strtoul(idbuf, &end, 16);
	if (!idbuf[0] || (end && *end))
		return -1;

	int eff = idlen > 3;
	if (eff) {
		if (id > CANDEV_EFF_MASK)
			return -1;
		f->can_id = (uint32_t)id | CANDEV_EFF_FLAG;
	} else {
		if (id > CANDEV_SFF_MASK)
			return -1;
		f->can_id = (uint32_t)id;
	}

	const char *rest = hash + 1;
	if (rest[0] == 'R' || rest[0] == 'r') {
		f->can_id |= CANDEV_RTR_FLAG;
		f->len = 0;
		return 0;
	}

	size_t hexlen = strlen(rest);
	if (hexlen % 2 != 0)
		return -1;
	size_t nbytes = hexlen / 2;
	if (nbytes > CANDEV_MAX_DLEN)
		return -1;
	for (size_t i = 0; i < nbytes; i++) {
		unsigned int byte;
		if (sscanf(rest + 2 * i, "%2x", &byte) != 1)
			return -1;
		f->data[i] = (uint8_t)byte;
	}
	f->len = (uint8_t)nbytes;
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	const char *slcan_hint = NULL;
	uint32_t bitrate = 500000;
	unsigned int repeat = 1, interval_ms = 0;

	static struct option opts[] = {
		{ "slcan-device", required_argument, 0, 'D' },
		{ "bitrate", required_argument, 0, 'r' },
		{ "repeat", required_argument, 0, 'R' },
		{ "interval-ms", required_argument, 0, 'i' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 },
	};

	int c;
	while ((c = getopt_long(argc, argv, "D:r:R:i:h", opts, NULL)) != -1) {
		switch (c) {
		case 'D': slcan_hint = optarg; break;
		case 'r': bitrate = (uint32_t)strtoul(optarg, NULL, 10); break;
		case 'R': repeat = (unsigned int)atoi(optarg); break;
		case 'i': interval_ms = (unsigned int)atoi(optarg); break;
		case 'h':
		default:
			fprintf(stderr,
				"usage: %s [--slcan-device /dev/ttyACM0] [opts] <id>#<hexdata|R> [...]\n"
				"  Tries a gs_usb-protocol adapter first (by known VID:PID), then falls\n"
				"  back to SLCAN (scanning /dev/ttyACM*, or --slcan-device if given).\n"
				"  --bitrate BPS    default 500000\n"
				"  --repeat N       resend the whole frame list N times (default 1)\n"
				"  --interval-ms N  delay between repeats\n"
				"example: %s 123#DEADBEEF\n",
				argv[0], argv[0]);
			return c == 'h' ? 0 : 1;
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "candev_send: no frames given, see --help\n");
		return 1;
	}

	int nframes = argc - optind;
	candev_frame *frames = calloc((size_t)nframes, sizeof(*frames));
	if (!frames) {
		fprintf(stderr, "candev_send: out of memory\n");
		return 1;
	}
	for (int i = 0; i < nframes; i++) {
		if (parse_frame(argv[optind + i], &frames[i]) != 0) {
			fprintf(stderr, "candev_send: cannot parse frame '%s'\n", argv[optind + i]);
			free(frames);
			return 1;
		}
	}

	char err[256];
	candev *dev = candev_open(slcan_hint, err, sizeof(err));
	if (!dev) {
		fprintf(stderr, "candev_send: %s\n", err);
		free(frames);
		return 1;
	}

	int rc = candev_set_bitrate(dev, bitrate);
	if (rc) {
		fprintf(stderr, "candev_send: set_bitrate(%u): %s\n", bitrate, candev_strerror(rc));
		candev_close(dev);
		free(frames);
		return 1;
	}

	rc = candev_start(dev, 0);
	if (rc) {
		fprintf(stderr, "candev_send: start: %s\n", candev_strerror(rc));
		candev_close(dev);
		free(frames);
		return 1;
	}

	fprintf(stderr, "candev_send: using %s backend\n", candev_backend_name(candev_which_backend(dev)));

	int ret = 0;
	for (unsigned int r = 0; r < repeat && ret == 0; r++) {
		for (int i = 0; i < nframes; i++) {
			rc = candev_send(dev, &frames[i], 1000);
			if (rc) {
				fprintf(stderr, "candev_send: send '%s': %s\n",
					argv[optind + i], candev_strerror(rc));
				ret = 1;
				break;
			}
		}
		if (interval_ms && r + 1 < repeat) {
			struct timespec ts = { .tv_sec = interval_ms / 1000,
					       .tv_nsec = (long)(interval_ms % 1000) * 1000000L };
			nanosleep(&ts, NULL);
		}
	}

	candev_stop(dev);
	candev_close(dev);
	free(frames);
	return ret;
}
