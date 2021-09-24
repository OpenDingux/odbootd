#include <errno.h>
#include <libusb-1.0/libusb.h>
#include <opk.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#include <math.h>
#endif

#ifdef _WIN32
// strndup() is not available on Windows
char *strndup( const char *s1, size_t n)
{
	if (strlen(s1) < n)
		return strdup(s1);
	char *copy= (char*)malloc( n+1 );
	memcpy( copy, s1, n );
	copy[n] = 0;
	return copy;
};
#endif

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define TIMEOUT_MS		10000

extern const char __end_image, __start_image;

struct board {
	const char *dts_code, *btl_code, *description;
};

struct board_group {
	const char *code;
	const struct board *boards;
	unsigned int num_boards;
	unsigned short vid, pid;
};

enum commands {
	CMD_GET_CPU_INFO,
	CMD_SET_DATA_ADDR,
	CMD_SET_DATA_LEN,
	CMD_FLUSH_CACHES,
	CMD_START1,
	CMD_START2,
};

enum custom_commands {
	CMD_EXIT,
	CMD_OPEN_FILE,
	CMD_CLOSE_FILE,
};

enum file_id {
	ID_ROOTFS,
	ID_UZIMAGE,
	ID_DTB,
	ID_UBIBOOT,
	ID_MININIT,
	ID_MODULESFS,
};

static const char *files_to_upload[] = {
	[ID_ROOTFS] = "rootfs.squashfs",
	[ID_UZIMAGE] = "uzImage.bin",
	[ID_MININIT] = "mininit-syspart",
	[ID_MODULESFS] = "modules.squashfs",
};

static const struct board gcw0_boards[] = {
	{ "gcw0_proto", "v11_ddr2_256mb", "GCW-Zero Prototype (256 MiB)" },
	{ "gcw0", "v20_mddr_512mb", "GCW-Zero" },
	{ "rg350", "rg350", "Anbernic RG-350 / RG-350P" },
	{ "rg350m", "rg350", "Anbernic RG-350M" },
	{ "rg280v", "rg350", "Anbernic RG-280V" },
	{ "rg280m", "rg350", "Anbernic RG-280M" },
	{ "rg300x", "rg350", "Anbernic RG-300X" },
	{ "pocketgo2", "v20_mddr_512mb", "Wolsen PocketGo2/PlayGo v1" },
	{ "pocketgo2v2", "rg350", "Wolsen PocketGo2/PlayGo v2" },
};

static const struct board rs90_boards[] = {
	{ "rs90", "v21", "Anbernic RS-90 v2.1" },
	{ "rs90", "v30", "Anbernic RS-90 v3.0" },
	{ "rg99", "v21", "Anbernic RG-99" },
};

static const struct board lepus_boards[] = {
	{ "rs97", "lepus", "Anbernic RS-97 v2.0" },
	{ "rg300", "lepus", "Anbernic RG-300 IPS / RS-97 IPS" },
	{ "ldkv", "lepus", "LDK (vertical)" },
	{ "ldkh", "lepus", "LDK (horizontal)" },
	{ "gopher2", "gopher2", "Gopher 2 JZ4760" },
	{ "gopher2b", "gopher2b", "Gopher 2 JZ4760B" },
};

static const struct board_group groups[] = {
	{
		.code = "gcw0",
		.boards = gcw0_boards,
		.num_boards = ARRAY_SIZE(gcw0_boards),
		.vid = 0xa108,
		.pid = 0x4770,
	},
	{
		.code = "rs90",
		.boards = rs90_boards,
		.num_boards = ARRAY_SIZE(rs90_boards),
		.vid = 0x601a,
		.pid = 0x4750,
	},
	{
		.code = "lepus",
		.boards = lepus_boards,
		.num_boards = ARRAY_SIZE(lepus_boards),
		.vid = 0x601a,
		.pid = 0x4760,
	},
};

static int get_device(const char *code, unsigned int *group, unsigned int *board)
{
	unsigned int i, j, choice = 0;

	for (i = 0; i < ARRAY_SIZE(groups); i++) {
		if (strcmp(groups[i].code, code))
			continue;

		printf("Flash which device?\n");

		for (j = 0; j < groups[i].num_boards; j++)
			printf("\t%u - %s\n", j + 1, groups[i].boards[j].description);

		do {
			printf("Your choice [1-%u]: ", groups[i].num_boards);
			while (scanf("%u", &choice) != 1);
		} while (choice < 1 || choice > groups[i].num_boards);

		*group = i;
		*board = choice - 1;
		return 0;
	}

	fprintf(stderr, "Unknown board codename %s\n", code);

	return -ENOENT;
}

static int cmd_get_info(libusb_device_handle *hdl)
{
	unsigned char info[8];

	int ret;

	ret = libusb_control_transfer(hdl, LIBUSB_ENDPOINT_IN |
			LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			CMD_GET_CPU_INFO, 0, 0, info, sizeof(info), TIMEOUT_MS);

	if (ret != sizeof(info))
		return -EIO;

	return 0;
}

static int cmd_control(libusb_device_handle *hdl, uint32_t cmd, uint32_t attr)
{
	return libusb_control_transfer(hdl, LIBUSB_ENDPOINT_OUT |
			LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			cmd, (attr >> 16) & 0xffff, attr & 0xffff,
			NULL, 0, TIMEOUT_MS);
}

static int cmd_control_iface(libusb_device_handle *hdl, uint8_t cmd, uint16_t attr)
{
	return libusb_control_transfer(hdl, LIBUSB_ENDPOINT_OUT |
			LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,
			cmd, attr, 0, NULL, 0, TIMEOUT_MS);
}

static int cmd_load_data(libusb_device_handle *hdl, unsigned char *data,
			 uint32_t addr, size_t size, bool stage1)
{
	int ret, bytes_transferred, to_transfer;
	size_t to_write;
	char *ptr;

	if (stage1) {
		/* Send the SET_DATA_LEN command */
		ret = cmd_control(hdl, CMD_SET_DATA_LEN, size);
		if (ret)
			return ret;

		/* Send the SET_DATA_ADDR command */
		ret = cmd_control(hdl, CMD_SET_DATA_ADDR, addr);
		if (ret)
			return ret;
	}

	ptr = (char *)data;
	to_write = size;

	do {
		if (to_write > 1024 * 1024)
			to_transfer = 1024 * 1024;
		else
			to_transfer = to_write;

		ret = libusb_bulk_transfer(hdl, LIBUSB_ENDPOINT_OUT | 0x1,
					   ptr, to_transfer, &bytes_transferred, 0);
		if (ret)
			return ret;

		to_write -= bytes_transferred;
		ptr += bytes_transferred;
	} while (to_write > 0);

	if (addr) {
		printf("Uploaded %lu bytes at address 0x%08x\n",
		       (unsigned long)size, addr);
	} else {
		printf("Uploaded %lu bytes\n", (unsigned long)size);
	}

	return 0;
}

static int cmd_load_file(libusb_device_handle *hdl, FILE *f,
		  uint32_t addr, size_t *data_size, bool stage1)
{
	size_t size, to_read;
	unsigned char *data;
	char *ptr;
	int ret;

	/* Get the file size */
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (data_size)
		*data_size = size;

	data = malloc(size);
	if (!data)
		return -ENOMEM;

	ptr = (char *)data;
	to_read = size;
	do {
		size_t bytes_read = fread(ptr, 1, to_read, f);
		if (!bytes_read) {
			ret = -EIO;
			goto out_free;
		}

		ptr += bytes_read;
		to_read -= bytes_read;
	} while (to_read > 0);

	ret = cmd_load_data(hdl, data, addr, size, stage1);

out_free:
	free(data);

	return ret;
}

static int cmd_load_from_file(libusb_device_handle *hdl, const char *fn,
			      uint32_t addr, size_t *data_size, bool stage1)
{
	FILE *f;
	int ret;

	f = fopen(fn, "rb");
	if (!f)
		return ferror(f);

	ret = cmd_load_file(hdl, f, addr, data_size, stage1);

	fclose(f);
	return ret;
}

static int load_from_opk(libusb_device_handle *hdl, struct OPK *opk,
			 const char *fn, enum file_id id)
{
	size_t data_size;
	uint32_t data_size32;
	void *data;
	int ret, bytes;

	ret = opk_extract_file(opk, fn, &data, &data_size);
	if (ret < 0) {
		if (ret != -ENOENT)
			fprintf(stderr, "Unable to extract data\n");
		return ret;
	}

	ret = cmd_control_iface(hdl, CMD_OPEN_FILE, id);
	if (ret) {
		fprintf(stderr, "Unable to send open: %i\n", ret);
		goto out_free_data;
	}

	data_size32 = data_size;

	ret = libusb_bulk_transfer(hdl, LIBUSB_ENDPOINT_OUT | 0x1,
			(unsigned char *)&data_size32, 4, &bytes, TIMEOUT_MS);
	if (ret) {
		fprintf(stderr, "Unable to write data size: %i\n", ret);
		goto out_free_data;
	}

	ret = cmd_load_data(hdl, data, 0x0, data_size, false);
	if (ret) {
		fprintf(stderr, "Unable to upload file: %i\n", ret);
		goto out_free_data;
	}

	ret = cmd_control_iface(hdl, CMD_CLOSE_FILE, 0);
	if (ret) {
		fprintf(stderr, "Unable to close!\n");
		goto out_free_data;
	}


out_free_data:
	free(data);
	return ret;
}

int main(int argc, char **argv)
{
	libusb_context *usb_ctx;
	libusb_device_handle *hdl;
	struct OPK *opk;
	unsigned int i, choice = 0;
	const char *fn, *firstdot, *lastdot;
	char *boardname, buf[256];
	unsigned int group, board;
	size_t data_size, kernel_size;
	void *data;
	int ret;

	// windows bundled libc with mingw does caching of buffers
	// this call disable caching
#ifdef _WIN32
	setbuf(stdout, NULL);
#endif

	if (HAS_BUILTIN_INSTALLER && argc != 2) {
		printf("Usage:\n\todboot-client od-update.opk\n");
		return EXIT_FAILURE;
	} else if (!HAS_BUILTIN_INSTALLER && argc != 3) {
		printf("Usage:\n\todboot-client od-update.opk vmlinuz.bin\n");
		return EXIT_FAILURE;
	}

	opk = opk_open(argv[1]);
	if (!opk) {
		fprintf(stderr, "Unable to open OPK file\n");
		return EXIT_FAILURE;
	}

	ret = opk_open_metadata(opk, &fn);
	if (ret <= 0)
		goto err_close_opk;

	firstdot = strchr(fn, '.');
	if (!firstdot)
		goto err_close_opk;

	lastdot = strchr(firstdot + 1, '.');
	if (!lastdot)
		goto err_close_opk;

	boardname = strndup(firstdot + 1, lastdot - firstdot - 1);

	ret = get_device(boardname, &group, &board);
	if (ret < 0)
		goto err_close_opk;

	ret = libusb_init(&usb_ctx);
	if (ret) {
		fprintf(stderr, "Unable to init libusb\n");
		return ret;
	}

	printf("trying to init device 0x%llx 0x%llx\n", groups[group].vid, groups[group].pid);

	hdl = libusb_open_device_with_vid_pid(usb_ctx,
					      groups[group].vid,
					      groups[group].pid);
	if (!hdl) {
		fprintf(stderr, "Unable to find Ingenic device.\n");
		ret = -ENOENT;
		goto out_exit_libusb;
	}

	ret = libusb_claim_interface(hdl, 0);
	if (ret) {
		fprintf(stderr, "Unable to claim interface 0\n");
		goto out_close_dev_handle;
	}

	if (cmd_get_info(hdl)) {
		fprintf(stderr, "Unable to read CPU info\n");
		goto out_close_dev_handle;
	}

	snprintf(buf, sizeof(buf), "%s/ubiboot-stage1-%s.bin", boardname,
		 groups[group].boards[board].btl_code);

	ret = opk_extract_file(opk, buf, &data, &data_size);
	if (ret < 0) {
		fprintf(stderr, "Unable to extract stage1 bootloader\n");
		goto err_close_opk;
	}

	ret = cmd_load_data(hdl, data, 0x80000000, data_size, true);
	free(data);

	if (ret) {
		fprintf(stderr, "Unable to upload stage1 bootloader\n");
		goto out_close_dev_handle;
	}

	printf("Uploaded bootloader\n");

	ret = cmd_control(hdl, CMD_START1, 0x80000000);
	if (ret) {
		fprintf(stderr, "Unable to execute stage1 bootloader\n");
		goto out_close_dev_handle;
	}

	/* Wait for stage1 to complete operation */
	for (i = 0; i < 100; i++) {
		if (!cmd_get_info(hdl))
			break;

		usleep(10000); /* 10ms * 100 = 1s */
	}

	if (i == 100) {
		fprintf(stderr, "Stage1 bootloader did not return.\n");
		goto out_close_dev_handle;
	}

	if (HAS_BUILTIN_INSTALLER) {
		kernel_size = (uintptr_t)&__end_image - (uintptr_t)&__start_image;
		ret = cmd_load_data(hdl, (unsigned char *)&__start_image,
				    0x81000000, kernel_size, true);
	} else {
		ret = cmd_load_from_file(hdl, argv[2], 0x81000000, &kernel_size, true);
	}
	if (ret) {
		fprintf(stderr, "Unable to upload kernel\n");
		goto out_close_dev_handle;
	}

	printf("Uploaded kernel\n");

	snprintf(buf, sizeof(buf), "%s/%s.dtb", boardname,
		 groups[group].boards[board].dts_code);

	ret = opk_extract_file(opk, buf, &data, &data_size);
	if (ret < 0) {
		fprintf(stderr, "Unable to extract DTB\n");
		goto err_close_opk;
	}

	ret = cmd_load_data(hdl, data, 0x81000000 + kernel_size, data_size, true);
	free(data);

	if (ret) {
		fprintf(stderr, "Unable to upload devicetree\n");
		goto out_close_dev_handle;
	}

	ret = cmd_control(hdl, CMD_FLUSH_CACHES, 0);
	if (ret) {
		fprintf(stderr, "Unable to flush caches\n");
		goto out_close_dev_handle;
	}

	ret = cmd_control(hdl, CMD_START2, 0x81000000);
	if (ret) {
		fprintf(stderr, "Unable to execute program\n");
		goto out_close_dev_handle;
	}

	printf("Operation suceeded.\n");

	/*
	 * The USB device will disconnect, and reconnect a bit later.
	 * Wait for the new USB device to appear.
	 */
	libusb_close(hdl);
	libusb_exit(usb_ctx);
	sleep(5);

	ret = libusb_init(&usb_ctx);
	if (ret) {
		fprintf(stderr, "Unable to init libusb\n");
		return ret;
	}

	for (;;) {
		hdl = libusb_open_device_with_vid_pid(usb_ctx,
						      groups[group].vid,
						      groups[group].pid);
		if (hdl)
			break;
		sleep(1);
	}

	ret = libusb_claim_interface(hdl, 0);
	if (ret) {
		fprintf(stderr, "Unable to claim interface 0\n");
		goto out_close_dev_handle;
	}

	for (i = 0; i < ARRAY_SIZE(files_to_upload); i++) {
		if (files_to_upload[i]) {
			snprintf(buf, sizeof(buf), "%s/%s",
				 boardname, files_to_upload[i]);
		} else if (i == ID_DTB) {
			snprintf(buf, sizeof(buf), "%s/%s.dtb",
				 boardname,
				 groups[group].boards[board].dts_code);
		} else if (i == ID_UBIBOOT) {
			snprintf(buf, sizeof(buf), "%s/ubiboot-%s.bin",
				 boardname,
				 groups[group].boards[board].btl_code);
		}

		ret = load_from_opk(hdl, opk, buf, i);
		if (ret) {
			if (ret == -ENOENT)
				continue;
			goto out_close_dev_handle;
		}
	}

	/* Exit */
	ret = cmd_control_iface(hdl, CMD_EXIT, 0);
	if (ret) {
		fprintf(stderr, "Unable to close!\n");
		goto out_close_dev_handle;
	}

out_close_dev_handle:
	libusb_close(hdl);
out_exit_libusb:
	libusb_exit(usb_ctx);
err_free_boardname:
	free(boardname);
err_close_opk:
	opk_close(opk);

	return EXIT_SUCCESS;
}
