/* slcan_send - cansend(1)-style frame transmit for SLCAN (serial-line CAN)
 * adapters. Frame syntax: <id>#{R|<hex bytes>}, e.g. "123#DEADBEEF" or
 * "123#R" -- same convention as this project's gsusb_send, minus the "##"
 * CAN-FD extension (SLCAN, per the reference driver, is classic-CAN only).
 * <id> with more than 3 hex digits is sent as an extended (29-bit) id.
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/slcan.h"

static int parse_frame(const char *s, slcan_frame *f)
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
		if (id > SLCAN_EFF_MASK)
			return -1;
		f->can_id = (uint32_t)id | SLCAN_EFF_FLAG;
	} else {
		if (id > SLCAN_SFF_MASK)
			return -1;
		f->can_id = (uint32_t)id;
	}

	const char *rest = hash + 1;
	if (rest[0] == 'R' || rest[0] == 'r') {
		f->can_id |= SLCAN_RTR_FLAG;
		f->len = 0;
		return 0;
	}

	size_t hexlen = strlen(rest);
	if (hexlen % 2 != 0)
		return -1;
	size_t nbytes = hexlen / 2;
	if (nbytes > SLCAN_MAX_DLEN)
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

	const char *device = NULL;
	uint32_t bitrate = 500000;
	unsigned int repeat = 1, interval_ms = 0;

	static struct option opts[] = {
		{ "device", required_argument, 0, 'D' },
		{ "bitrate", required_argument, 0, 'r' },
		{ "repeat", required_argument, 0, 'R' },
		{ "interval-ms", required_argument, 0, 'i' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 },
	};

	int c;
	while ((c = getopt_long(argc, argv, "D:r:R:i:h", opts, NULL)) != -1) {
		switch (c) {
		case 'D': device = optarg; break;
		case 'r': bitrate = (uint32_t)strtoul(optarg, NULL, 10); break;
		case 'R': repeat = (unsigned int)atoi(optarg); break;
		case 'i': interval_ms = (unsigned int)atoi(optarg); break;
		case 'h':
		default:
			fprintf(stderr,
				"usage: %s --device /dev/ttyACM0 [opts] <id>#<hexdata|R> [<id>#<hexdata|R> ...]\n"
				"  --device PATH    serial device the adapter is on (required)\n"
				"  --bitrate BPS    default 500000\n"
				"  --repeat N       resend the whole frame list N times (default 1)\n"
				"  --interval-ms N  delay between repeats\n"
				"example: %s --device /dev/ttyACM0 123#DEADBEEF\n",
				argv[0], argv[0]);
			return c == 'h' ? 0 : 1;
		}
	}

	if (!device) {
		fprintf(stderr, "slcan_send: --device is required (e.g. --device /dev/ttyACM0)\n");
		return 1;
	}
	if (optind >= argc) {
		fprintf(stderr, "slcan_send: no frames given, see --help\n");
		return 1;
	}

	int nframes = argc - optind;
	slcan_frame *frames = calloc((size_t)nframes, sizeof(*frames));
	if (!frames) {
		fprintf(stderr, "slcan_send: out of memory\n");
		return 1;
	}
	for (int i = 0; i < nframes; i++) {
		if (parse_frame(argv[optind + i], &frames[i]) != 0) {
			fprintf(stderr, "slcan_send: cannot parse frame '%s'\n", argv[optind + i]);
			free(frames);
			return 1;
		}
	}

	char err[256];
	slcan_dev *dev = slcan_open(device, err, sizeof(err));
	if (!dev) {
		fprintf(stderr, "slcan_send: %s\n", err);
		free(frames);
		return 1;
	}

	int rc = slcan_set_bitrate(dev, bitrate);
	if (rc) {
		fprintf(stderr, "slcan_send: set_bitrate(%u): %s\n", bitrate, slcan_strerror(rc));
		slcan_close(dev);
		free(frames);
		return 1;
	}

	rc = slcan_start(dev, 0);
	if (rc) {
		fprintf(stderr, "slcan_send: start: %s\n", slcan_strerror(rc));
		slcan_close(dev);
		free(frames);
		return 1;
	}

	int ret = 0;
	for (unsigned int r = 0; r < repeat && ret == 0; r++) {
		for (int i = 0; i < nframes; i++) {
			rc = slcan_send(dev, &frames[i], 1000);
			if (rc) {
				fprintf(stderr, "slcan_send: send '%s': %s\n",
					argv[optind + i], slcan_strerror(rc));
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

	slcan_stop(dev);
	slcan_close(dev);
	free(frames);
	return ret;
}
