/* gcan_native_tool - tx/rx CLI for libgcan_native, mostly for manual
 * hardware verification. See ../include/gcan_native.h for the library API
 * this wraps, and ../README.md for the reverse-engineered protocol behind
 * it and its current limitations (channel 0, 500 kbit/s, classic CAN
 * only).
 */
#include "../include/gcan_native.h"

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

static int parse_data_hex(const char *input, uint8_t *data)
{
	char clean[64];
	int n = 0;
	for (int i = 0; input[i] != '\0' && n < (int)sizeof(clean) - 1; i++)
		if (isxdigit((unsigned char)input[i]))
			clean[n++] = input[i];
	clean[n] = '\0';

	if (n == 0 || (n % 2) != 0) {
		fprintf(stderr, "ERROR: data hex length must be even\n");
		return -1;
	}
	int len = n / 2;
	if (len > GCAN_NATIVE_MAX_DLEN) {
		fprintf(stderr, "ERROR: classic CAN max data is %d bytes\n", GCAN_NATIVE_MAX_DLEN);
		return -1;
	}
	for (int i = 0; i < len; i++) {
		char b[3] = { clean[i * 2], clean[i * 2 + 1], 0 };
		data[i] = (uint8_t)strtoul(b, NULL, 16);
	}
	return len;
}

static void usage(const char *prog)
{
	printf("\n"
	       "gcan_native_tool -- libusb-1.0 only, no vendor libECanVci.so\n"
	       "\n"
	       "Usage:\n"
	       "  %s tx --id <hex> --data <hex> [--ext] [--count N]\n"
	       "  %s rx\n"
	       "\n"
	       "Current limitation: channel 0 only, 500 kbit/s only, classic CAN only\n"
	       "\n",
	       prog, prog);
}

int main(int argc, char **argv)
{
	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	int mode_tx = strcmp(argv[1], "tx") == 0;
	int mode_rx = strcmp(argv[1], "rx") == 0;
	if (!mode_tx && !mode_rx) {
		usage(argv[0]);
		return 1;
	}

	uint32_t can_id = 0;
	int has_id = 0, ext = 0, count = 1;
	const char *data_hex = NULL;

	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
			can_id = (uint32_t)strtoul(argv[++i], NULL, 16);
			has_id = 1;
		} else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
			data_hex = argv[++i];
		} else if (strcmp(argv[i], "--ext") == 0) {
			ext = 1;
		} else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
			count = atoi(argv[++i]);
			if (count < 1) count = 1;
		} else if (strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "ERROR: unknown argument: %s\n", argv[i]);
			usage(argv[0]);
			return 1;
		}
	}

	gcan_native_frame frame = { 0 };
	if (mode_tx) {
		if (!has_id || !data_hex) {
			fprintf(stderr, "ERROR: tx requires --id and --data\n");
			return 1;
		}
		if (!ext && can_id > GCAN_NATIVE_SFF_MASK) {
			fprintf(stderr, "ERROR: standard CAN id max is 0x7FF; use --ext for 29-bit\n");
			return 1;
		}
		if (ext && can_id > GCAN_NATIVE_EFF_MASK) {
			fprintf(stderr, "ERROR: extended CAN id max is 0x1FFFFFFF\n");
			return 1;
		}
		int dlc = parse_data_hex(data_hex, frame.data);
		if (dlc < 0)
			return 1;
		frame.can_id = can_id | (ext ? GCAN_NATIVE_EFF_FLAG : 0);
		frame.len = (uint8_t)dlc;
	}

	void *ctx;
	int rc = gcan_native_init(&ctx);
	if (rc != GCAN_NATIVE_OK) {
		fprintf(stderr, "ERROR: init: %s\n", gcan_native_strerror(rc));
		return 1;
	}

	char errbuf[256];
	gcan_native_dev *dev = gcan_native_open(ctx, errbuf, sizeof(errbuf));
	if (!dev) {
		fprintf(stderr, "ERROR: %s\n", errbuf);
		gcan_native_exit(ctx);
		return 1;
	}

	gcan_native_channel *ch = gcan_native_channel_get(dev, 0);
	if (!ch) {
		fprintf(stderr, "ERROR: channel 0 not available\n");
		gcan_native_close(dev);
		gcan_native_exit(ctx);
		return 1;
	}

	rc = gcan_native_channel_set_bitrate(ch, 500000, 0.0);
	if (rc != GCAN_NATIVE_OK) {
		fprintf(stderr, "ERROR: set_bitrate: %s\n", gcan_native_strerror(rc));
		gcan_native_close(dev);
		gcan_native_exit(ctx);
		return 1;
	}
	rc = gcan_native_channel_start(ch, 0);
	if (rc != GCAN_NATIVE_OK) {
		fprintf(stderr, "ERROR: start: %s\n", gcan_native_strerror(rc));
		gcan_native_close(dev);
		gcan_native_exit(ctx);
		return 1;
	}

	int ok = 1;
	if (mode_tx) {
		for (int i = 0; i < count && ok; i++) {
			rc = gcan_native_channel_send(ch, &frame, 1000);
			if (rc != GCAN_NATIVE_OK) {
				fprintf(stderr, "ERROR: send: %s\n", gcan_native_strerror(rc));
				ok = 0;
				break;
			}
			printf("TX %d/%d id=0x%X ext=%d dlc=%d\n", i + 1, count, can_id, ext, frame.len);
		}
	} else {
		printf("Listening on channel 0 @ 500 kbit/s, Ctrl-C to stop\n");
		while (running) {
			gcan_native_frame rx;
			rc = gcan_native_channel_recv(ch, &rx, 200);
			if (rc < 0) {
				fprintf(stderr, "ERROR: recv: %s\n", gcan_native_strerror(rc));
				ok = 0;
				break;
			}
			if (rc == 0)
				continue;
			int rx_ext = (rx.can_id & GCAN_NATIVE_EFF_FLAG) ? 1 : 0;
			uint32_t rx_id = rx.can_id & (rx_ext ? GCAN_NATIVE_EFF_MASK : GCAN_NATIVE_SFF_MASK);
			printf("RX id=0x%X ext=%d dlc=%d data=", rx_id, rx_ext, rx.len);
			for (int i = 0; i < rx.len; i++)
				printf("%02X%s", rx.data[i], i + 1 < rx.len ? " " : "");
			printf("\n");
			fflush(stdout);
		}
	}

	gcan_native_channel_stop(ch);
	gcan_native_close(dev);
	gcan_native_exit(ctx);
	return ok ? 0 : 1;
}
