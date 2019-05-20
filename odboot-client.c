#include <errno.h>
#include <getopt.h>
#include <libusb-1.0/libusb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MY_NAME			"odboot"

#define INGENIC_VENDOR_ID	0x601A

#define TIMEOUT_MS		5000

#define ARRAY_SIZE(x) (sizeof(x) ? sizeof(x) / sizeof((x)[0]) : 0)

enum commands {
	CMD_EXIT,
	CMD_OPEN_FILE,
	CMD_CLOSE_FILE,
};

static const uint16_t ingenic_product_ids[] = {
	0x4740,
	0x4750,
	0x4770,
	0x4780,
};

static const struct option options[] = {
	{"help", no_argument, 0, 'h'},
	{0, 0, 0, 0},
};

static const char *options_descriptions[] = {
	"Show this help and quit.",
};

static void usage(void)
{
	unsigned int i;

	printf("Usage:\n\t" MY_NAME " [OPTIONS ...] [rootfs] [kernel] [devicetree] [ubiboot] [mininit]\n\nOptions:\n");
	for (i = 0; options[i].name; i++)
		printf("\t-%c, --%s\n\t\t\t%s\n",
					options[i].val, options[i].name,
					options_descriptions[i]);
}

static int cmd_control(libusb_device_handle *hdl, uint8_t cmd, uint16_t attr)
{
	return libusb_control_transfer(hdl, LIBUSB_ENDPOINT_OUT |
			LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,
			cmd, attr, 0, NULL, 0, TIMEOUT_MS);
}

static int cmd_load_data(libusb_device_handle *hdl, FILE *f, size_t *data_size)
{
	int ret, bytes_transferred;
	size_t size, bytes_left;
	unsigned char *data, *ptr;

	/* Get the file size */
	fseek(f, 0, SEEK_END);
	size = bytes_left = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (data_size)
		*data_size = size;

	data = malloc(1024 * 1024);
	if (!data)
		return -ENOMEM;

	while (bytes_left) {
		size_t bytes_read, to_read = bytes_left;

		if (to_read > 1024 * 1024)
			to_read = 1024 * 1024;

		bytes_read = fread(data, 1, to_read, f);
		if (!bytes_read) {
			ret = -EIO;
			goto out_free;
		}

		for (ptr = data; bytes_read; ) {
			ret = libusb_bulk_transfer(hdl,
						   LIBUSB_ENDPOINT_OUT | 0x1,
						   ptr, (int)bytes_read,
						   &bytes_transferred, 0);
			if (ret)
				goto out_free;

			bytes_left -= bytes_transferred;
			bytes_read -= bytes_transferred;
			ptr += bytes_transferred;
		}
	}

	printf("Uploaded %lu bytes\n", (unsigned long)size);

out_free:
	free(data);
	return ret;
}

static int upload_file(libusb_device_handle *hdl, FILE *f, unsigned int id)
{
	uint32_t data_size;
	int ret, bytes_transferred;

	fseek(f, 0, SEEK_END);
	data_size = ftell(f);
	fseek(f, 0, SEEK_SET);

	ret = cmd_control(hdl, CMD_OPEN_FILE, id);
	if (ret) {
		fprintf(stderr, "Unable to send open: %i\n", ret);
		return ret;
	}

	ret = libusb_bulk_transfer(hdl, LIBUSB_ENDPOINT_OUT | 0x1,
			(unsigned char *)&data_size, 4, &bytes_transferred, TIMEOUT_MS);
	if (ret) {
		fprintf(stderr, "Unable to write data size: %i\n", ret);
		return ret;
	}

	ret = cmd_load_data(hdl, f, NULL);
	if (ret) {
		fprintf(stderr, "Unable to upload file: %i\n", ret);
		return ret;
	}

	ret = cmd_control(hdl, CMD_CLOSE_FILE, 0);
	if (ret) {
		fprintf(stderr, "Unable to close!\n");
		return ret;
	}

	return 0;
}

int main(int argc, char **argv)
{
	libusb_context *usb_ctx;
	libusb_device_handle *hdl = NULL;
	int ret, exit_code = EXIT_FAILURE;
	unsigned int i;
	uint16_t pid;

	if (argc == 1) {
		usage();
		return EXIT_FAILURE;
	}

	if (!strcmp("-h", argv[1]) || !strcmp("--help", argv[1])) {
		usage();
		return EXIT_SUCCESS;
	}

	ret = libusb_init(&usb_ctx);
	if (ret) {
		fprintf(stderr, "Unable to init libusb\n");
		return EXIT_FAILURE;
	}

	printf("Detecting OpenDingux...\n");

	for (;;) {
		for (i = 0; !hdl && i < ARRAY_SIZE(ingenic_product_ids); i++) {
			hdl = libusb_open_device_with_vid_pid(usb_ctx,
				INGENIC_VENDOR_ID, ingenic_product_ids[i]);
		}

		if (hdl)
			break;

		sleep(1);
	}

	pid = ingenic_product_ids[i - 1];

	ret = libusb_claim_interface(hdl, 0);
	if (ret) {
		fprintf(stderr, "Unable to claim interface 0\n");
		goto out_close_dev_handle;
	}

	printf("Found Ingenic JZ%x based device\n", pid);

	for (i = 0; i < argc - 1; i++) {
		FILE *f = fopen(argv[i + 1], "rb");

		if (!f) {
			fprintf(stderr, "Unable to open file %s\n",
				argv[i + 1]);
			continue;
		}

		upload_file(hdl, f, i);
		fclose(f);
	}

	/* Exit */
	ret = cmd_control(hdl, CMD_EXIT, 0);
	if (ret) {
		fprintf(stderr, "Unable to close!\n");
		goto out_close_dev_handle;
	}

	printf("Operation complete.\n");
	exit_code = EXIT_SUCCESS;
out_close_dev_handle:
	libusb_close(hdl);
	libusb_exit(usb_ctx);
	return exit_code;
}
