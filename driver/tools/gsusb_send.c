/* gsusb_send - cansend(1)-style single/repeated CAN frame transmit for
 * gs_usb devices.
 *
 * Frame syntax (same as can-utils' cansend):
 *   <id>#{R|<hex bytes>}          classic CAN, e.g. "123#DEADBEEF" or "123#R"
 *   <id>##<flags><hex bytes>      CAN-FD, <flags> is one hex nibble:
 *                                 bit0=BRS, bit1=ESI, e.g. "123##1AABBCCDD"
 * <id> with more than 3 hex digits is sent as an extended (29-bit) id.
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common.h"

static int parse_frame(const char *s, gsusb_frame *f)
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

	uint32_t id;
	if (gsusb_parse_hex_u32(idbuf, &id) != 0)
		return -1;

	int eff = idlen > 3;
	if (eff) {
		if (id > GSUSB_EFF_MASK)
			return -1;
		f->can_id = id | GSUSB_EFF_FLAG;
	} else {
		if (id > GSUSB_SFF_MASK)
			return -1;
		f->can_id = id;
	}

	int fd = 0;
	const char *rest;
	if (hash[1] == '#') {
		fd = 1;
		rest = hash + 2;
	} else {
		rest = hash + 1;
	}

	if (fd) {
		if (!rest[0])
			return -1;
		char c = rest[0];
		int flagsval;
		if (c >= '0' && c <= '9') flagsval = c - '0';
		else if (c >= 'a' && c <= 'f') flagsval = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') flagsval = c - 'A' + 10;
		else return -1;

		f->flags |= GSUSB_FRAME_FD;
		if (flagsval & 0x1) f->flags |= GSUSB_FRAME_BRS;
		if (flagsval & 0x2) f->flags |= GSUSB_FRAME_ESI;
		rest++;

		size_t hexlen = strlen(rest);
		if (hexlen % 2 != 0)
			return -1;
		size_t nbytes = hexlen / 2;
		if (nbytes > GSUSB_MAX_DLEN)
			return -1;
		for (size_t i = 0; i < nbytes; i++) {
			unsigned int byte;
			if (sscanf(rest + 2 * i, "%2x", &byte) != 1)
				return -1;
			f->data[i] = (uint8_t)byte;
		}
		f->len = (uint8_t)nbytes;
	} else if (rest[0] == 'R' || rest[0] == 'r') {
		f->can_id |= GSUSB_RTR_FLAG;
		f->len = 0;
	} else {
		size_t hexlen = strlen(rest);
		if (hexlen % 2 != 0)
			return -1;
		size_t nbytes = hexlen / 2;
		if (nbytes > 8)
			return -1;
		for (size_t i = 0; i < nbytes; i++) {
			unsigned int byte;
			if (sscanf(rest + 2 * i, "%2x", &byte) != 1)
				return -1;
			f->data[i] = (uint8_t)byte;
		}
		f->len = (uint8_t)nbytes;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int vid = 0, pid = 0, bus = -1, addr = -1;
	unsigned int channel = 0;
	uint32_t bitrate = 500000, data_bitrate = 2000000;
	int fd_mode = 0, loopback = 0;
	unsigned int repeat = 1, interval_ms = 0;

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
		{ "repeat", required_argument, 0, 'R' },
		{ "interval-ms", required_argument, 0, 'i' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 },
	};

	int c;
	while ((c = getopt_long(argc, argv, "v:p:b:a:c:r:d:fLR:i:h", opts, NULL)) != -1) {
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
		case 'R': repeat = (unsigned int)atoi(optarg); break;
		case 'i': interval_ms = (unsigned int)atoi(optarg); break;
		case 'h':
		default:
			fprintf(stderr,
				"usage: %s [opts] <id>#<hexdata|R> [<id>#<hexdata|R> ...]\n"
				"       %s [opts] --fd <id>##<flags><hexdata> ...\n"
				"  --vid/--pid HEX, --bus/--addr N, --channel N\n"
				"  --bitrate BPS (default 500000), --fd --data-bitrate BPS (default 2000000)\n"
				"  --loopback      also loop the frame(s) back to this adapter's own rx\n"
				"  --repeat N      resend the whole frame list N times (default 1)\n"
				"  --interval-ms N delay between repeats\n"
				"example: %s 123#DEADBEEF\n"
				"example: %s --fd 1FFFFFFF##1 0011223344556677\n",
				argv[0], argv[0], argv[0], argv[0]);
			return c == 'h' ? 0 : 1;
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "gsusb_send: no frames given, see --help\n");
		return 1;
	}

	int nframes = argc - optind;
	gsusb_frame *frames = calloc((size_t)nframes, sizeof(*frames));
	if (!frames) {
		fprintf(stderr, "gsusb_send: out of memory\n");
		return 1;
	}
	for (int i = 0; i < nframes; i++) {
		if (parse_frame(argv[optind + i], &frames[i]) != 0) {
			fprintf(stderr, "gsusb_send: cannot parse frame '%s'\n", argv[optind + i]);
			return 1;
		}
		if (frames[i].flags & GSUSB_FRAME_FD)
			fd_mode = 1;
	}

	void *ctx = NULL;
	int rc = gsusb_init(&ctx);
	if (rc) {
		fprintf(stderr, "gsusb_send: %s\n", gsusb_strerror(rc));
		return 1;
	}

	gsusb_dev *dev = gsusb_tool_open(ctx, vid, pid, bus, addr);
	gsusb_channel *ch = gsusb_channel_get(dev, channel);
	if (!ch) {
		fprintf(stderr, "gsusb_send: channel %u does not exist on this device\n", channel);
		return 1;
	}
	if (fd_mode && !(gsusb_channel_features(ch) & GSUSB_FEATURE_FD)) {
		fprintf(stderr, "gsusb_send: channel %u does not support CAN-FD\n", channel);
		return 1;
	}

	rc = gsusb_channel_set_bitrate(ch, bitrate, 0);
	if (rc) {
		fprintf(stderr, "gsusb_send: set_bitrate(%u): %s\n", bitrate, gsusb_strerror(rc));
		return 1;
	}
	if (fd_mode) {
		rc = gsusb_channel_set_data_bitrate(ch, data_bitrate, 0);
		if (rc) {
			fprintf(stderr, "gsusb_send: set_data_bitrate(%u): %s\n",
				data_bitrate, gsusb_strerror(rc));
			return 1;
		}
	}

	uint32_t mode = 0;
	if (fd_mode) mode |= GSUSB_MODE_FD;
	if (loopback) mode |= GSUSB_MODE_LOOPBACK;

	rc = gsusb_channel_start(ch, mode);
	if (rc) {
		fprintf(stderr, "gsusb_send: start: %s\n", gsusb_strerror(rc));
		return 1;
	}

	int ret = 0;
	for (unsigned int r = 0; r < repeat && ret == 0; r++) {
		for (int i = 0; i < nframes; i++) {
			rc = gsusb_channel_send(ch, &frames[i], 1000);
			if (rc) {
				fprintf(stderr, "gsusb_send: send '%s': %s\n",
					argv[optind + i], gsusb_strerror(rc));
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

	gsusb_channel_stop(ch);
	gsusb_close(dev);
	gsusb_exit(ctx);
	free(frames);
	return ret;
}
