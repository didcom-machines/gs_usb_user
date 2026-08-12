/* gsusb_info - list gs_usb-compatible USB-CAN adapters and print their
 * capabilities (channel count, bit-timing constants, supported features).
 */
#include <getopt.h>
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

static void print_features(uint32_t f)
{
	static const struct { uint32_t bit; const char *name; } tbl[] = {
		{ GSUSB_FEATURE_LISTEN_ONLY, "listen-only" },
		{ GSUSB_FEATURE_LOOP_BACK, "loopback" },
		{ GSUSB_FEATURE_TRIPLE_SAMPLE, "triple-sample" },
		{ GSUSB_FEATURE_ONE_SHOT, "one-shot" },
		{ GSUSB_FEATURE_HW_TIMESTAMP, "hw-timestamp" },
		{ GSUSB_FEATURE_IDENTIFY, "identify" },
		{ GSUSB_FEATURE_USER_ID, "user-id" },
		{ GSUSB_FEATURE_PAD_PKTS_TO_MAX, "pad-pkts" },
		{ GSUSB_FEATURE_FD, "canfd" },
		{ GSUSB_FEATURE_BT_CONST_EXT, "bt-const-ext" },
		{ GSUSB_FEATURE_TERMINATION, "termination" },
		{ GSUSB_FEATURE_BERR_REPORTING, "berr-reporting" },
		{ GSUSB_FEATURE_GET_STATE, "get-state" },
	};
	int first = 1;
	for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
		if (f & tbl[i].bit) {
			printf("%s%s", first ? "" : " ", tbl[i].name);
			first = 0;
		}
	}
	if (first)
		printf("(none)");
	printf("\n");
}

static void print_bt_const(const gsusb_bittiming_const *btc, const char *label)
{
	printf("    %s: clock=%u Hz  tseg1=%u..%u  tseg2=%u..%u  sjw_max=%u  brp=%u..%u step %u\n",
	       label, btc->fclk_can, btc->tseg1_min, btc->tseg1_max,
	       btc->tseg2_min, btc->tseg2_max, btc->sjw_max,
	       btc->brp_min, btc->brp_max, btc->brp_inc);
}

static void do_scan(int vid, int pid, int bus_filter, int addr_filter)
{
	libusb_context *ctx = NULL;
	if (libusb_init(&ctx) != 0) {
		fprintf(stderr, "gsusb_info: libusb_init failed\n");
		exit(1);
	}

	libusb_device **list = NULL;
	ssize_t n = libusb_get_device_list(ctx, &list);
	if (n < 0) {
		fprintf(stderr, "gsusb_info: libusb_get_device_list failed: %s\n",
			libusb_strerror((int)n));
		exit(1);
	}

	int found = 0;
	for (ssize_t i = 0; i < n; i++) {
		struct libusb_device_descriptor desc;
		if (libusb_get_device_descriptor(list[i], &desc) != 0)
			continue;

		const char *name = NULL;
		if (vid > 0 && pid > 0) {
			if (desc.idVendor != vid || desc.idProduct != pid)
				continue;
			name = "user-specified";
		} else {
			for (size_t k = 0; k < GSUSB_KNOWN_ID_COUNT; k++) {
				if (desc.idVendor == gsusb_known_ids[k].vid &&
				    desc.idProduct == gsusb_known_ids[k].pid) {
					name = gsusb_known_ids[k].name;
					break;
				}
			}
			if (!name)
				continue;
		}

		uint8_t bus = libusb_get_bus_number(list[i]);
		uint8_t addr = libusb_get_device_address(list[i]);
		if (bus_filter >= 0 && bus != (uint8_t)bus_filter)
			continue;
		if (addr_filter >= 0 && addr != (uint8_t)addr_filter)
			continue;
		char manuf[128] = "", prod[128] = "";

		libusb_device_handle *h = NULL;
		if (libusb_open(list[i], &h) == 0) {
			if (desc.iManufacturer)
				libusb_get_string_descriptor_ascii(h, desc.iManufacturer,
					(unsigned char *)manuf, sizeof(manuf));
			if (desc.iProduct)
				libusb_get_string_descriptor_ascii(h, desc.iProduct,
					(unsigned char *)prod, sizeof(prod));
			libusb_close(h);
		}

		printf("bus %03u addr %03u  %04x:%04x  %s\n",
		       bus, addr, desc.idVendor, desc.idProduct, name);
		if (manuf[0] || prod[0])
			printf("    %s%s%s\n", manuf, (manuf[0] && prod[0]) ? " " : "", prod);
		found++;
	}

	if (!found)
		printf("no matching devices found\n");

	libusb_free_device_list(list, 1);
	libusb_exit(ctx);
}

int main(int argc, char **argv)
{
	int vid = 0, pid = 0, bus = -1, addr = -1, scan = 0;

	static struct option opts[] = {
		{ "vid", required_argument, 0, 'v' },
		{ "pid", required_argument, 0, 'p' },
		{ "bus", required_argument, 0, 'b' },
		{ "addr", required_argument, 0, 'a' },
		{ "scan", no_argument, 0, 's' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 },
	};

	int c;
	while ((c = getopt_long(argc, argv, "v:p:b:a:sh", opts, NULL)) != -1) {
		switch (c) {
		case 'v': vid = (int)strtoul(optarg, NULL, 16); break;
		case 'p': pid = (int)strtoul(optarg, NULL, 16); break;
		case 'b': bus = atoi(optarg); break;
		case 'a': addr = atoi(optarg); break;
		case 's': scan = 1; break;
		case 'h':
		default:
			fprintf(stderr,
				"usage: %s [--scan] [--vid HEX --pid HEX] [--bus N --addr N]\n"
				"  --scan          list matching devices without opening them\n"
				"  --vid/--pid     restrict to a specific USB vendor:product id (hex)\n"
				"  --bus/--addr    restrict to a specific USB bus number / device address\n"
				"With no arguments, opens the first known gs_usb-compatible device found\n"
				"and prints its channel count and per-channel bit-timing capabilities.\n",
				argv[0]);
			return c == 'h' ? 0 : 1;
		}
	}

	if (scan) {
		do_scan(vid, pid, bus, addr);
		return 0;
	}

	void *ctx = NULL;
	int rc = gsusb_init(&ctx);
	if (rc) {
		fprintf(stderr, "gsusb_info: %s\n", gsusb_strerror(rc));
		return 1;
	}

	gsusb_dev *dev = gsusb_tool_open(ctx, vid, pid, bus, addr);
	const gsusb_device_config *dc = gsusb_get_device_config(dev);

	printf("device: sw_version=%u hw_version=%u channels=%u\n",
	       dc->sw_version, dc->hw_version, dc->channel_count);

	for (unsigned int i = 0; i < gsusb_channel_count(dev); i++) {
		gsusb_channel *ch = gsusb_channel_get(dev, i);
		uint32_t feature = gsusb_channel_features(ch);

		printf("  channel %u: features: ", i);
		print_features(feature);

		print_bt_const(gsusb_channel_bt_const(ch), "nominal bit timing");
		const gsusb_bittiming_const *dbtc = gsusb_channel_data_bt_const(ch);
		if (dbtc)
			print_bt_const(dbtc, "data-phase timing  ");
		else if (feature & GSUSB_FEATURE_FD)
			printf("    data-phase timing  : shares nominal timing constants above\n");

		if (feature & GSUSB_FEATURE_TERMINATION) {
			int on = 0;
			if (gsusb_channel_get_termination(ch, &on) == 0)
				printf("    termination: %s\n", on ? "120 ohm enabled" : "disabled");
		}
	}

	gsusb_close(dev);
	gsusb_exit(ctx);
	return 0;
}
