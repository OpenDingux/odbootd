/*
 * jzbootd - FunctionFS daemon to comunicate with jzboot-gtk
 *
 * USB code based on Libiio (Copyright (C) 2016 Analog Devices, Inc.)
 *
 * Licensed under the GPLv2
 */

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/functionfs.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define NAME u8"JZBOOT"

#define LE32(x) ((__BYTE_ORDER != __BIG_ENDIAN) ? (x) : __builtin_bswap32(x))
#define LE16(x) ((__BYTE_ORDER != __BIG_ENDIAN) ? (x) : __builtin_bswap16(x))

enum jzboot_commands {
	CMD_EXIT,
	CMD_OPEN_FILE,
	CMD_CLOSE_FILE,
};

struct usb_ffs_header {
	struct usb_functionfs_descs_head_v2 header;
	uint32_t nb_fs, nb_hs, nb_ss;
} __attribute__((packed));

struct usb_ffs_strings {
	struct usb_functionfs_strings_head head;
	uint16_t lang;
	const char string[sizeof(NAME)];
} __attribute__((packed));

struct pdata {
	pthread_t thd;
	int data_fd;
	int ep1_fd;
};

static const struct usb_ffs_strings ffs_strings = {
	.head = {
		.magic = LE32(FUNCTIONFS_STRINGS_MAGIC),
		.length = LE32(sizeof(ffs_strings)),
		.str_count = LE32(1),
		.lang_count = LE32(1),
	},

	.lang = LE16(0x409),
	.string = NAME,
};

static int stop_fd;

static inline int poll_nointr(struct pollfd *pfd, unsigned int num_pfd)
{
	int ret;

	do {
		ret = poll(pfd, num_pfd, -1);
	} while (ret == -1 && errno == EINTR);

	return ret;
}

static const char * const jzboot_file_paths[] = {
	"/boot/rootfs.squashfs",
	"/boot/uzImage.bin",
	"/boot/devicetree.dtb",
};

static void * jzboot_read_data(void *d)
{
	struct pdata *pdata = d;
	uint32_t data_size;
	ssize_t ret;
	char buf[4096];

	ret = read(pdata->ep1_fd, (unsigned char *)&data_size,
		   sizeof(&data_size));
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "Unable to read data size: %s\n",
			strerror(-ret));
		return (void *)ret;
	}

	printf("Data size: %u bytes\n", data_size);

	while (data_size) {
		uint32_t bytes_read, to_read = data_size;

		if (to_read > sizeof(buf))
			to_read = sizeof(buf);

		ret = read(pdata->ep1_fd, buf, to_read);
		if (ret == -1) {
			ret = -errno;
			break;
		}

		bytes_read = ret;

		ret = write(pdata->data_fd, buf, bytes_read);
		if (ret == -1) {
			ret = -errno;
			break;
		}

		data_size -= ret;
		ret = 0;
	}

	return (void *)ret;
}

static int jzboot_open_file(struct pdata *pdata,
			    const struct usb_ctrlrequest *req)
{
	const char *fn = jzboot_file_paths[le16toh(req->wValue)];
	int ret;

	printf("Opening file: %s\n", fn);

	ret = open(fn, O_WRONLY | O_TRUNC | O_CREAT);
	if (ret == -1)
		return -errno;

	pdata->data_fd = ret;

	ret = pthread_create(&pdata->thd, NULL, jzboot_read_data, pdata);
	if (ret) {
		close(pdata->data_fd);
		return ret;
	}

	return 0;
}

static void jzboot_close_file(struct pdata *pdata,
			      const struct usb_ctrlrequest *req)
{
	int retval;

	pthread_join(pdata->thd, (void **)&retval);

	close(pdata->data_fd);
	pdata->data_fd = -1;

	if (retval)
		printf("Read thread exited with status %i\n", retval);
}

static void jzboot_exit(void)
{
	uint64_t e = 1;
	int ret;

	do {
		ret = write(stop_fd, &e, sizeof(e));
	} while (ret == -1 && errno == EINTR);
}

static int handle_event(struct pdata *pdata,
			const struct usb_functionfs_event *event)
{
	int ret = 0;

	if (event->type == FUNCTIONFS_SETUP) {
		const struct usb_ctrlrequest *req = &event->u.setup;

		switch (req->bRequest) {
		case CMD_EXIT:
			jzboot_exit();
			break;
		case CMD_OPEN_FILE:
			ret = jzboot_open_file(pdata, req);
			break;
		case CMD_CLOSE_FILE:
			jzboot_close_file(pdata, req);
			break;
		}
	}

	return ret;
}

static struct usb_ffs_header * create_header(uint32_t size)
{
	/* Packet sizes for USB high-speed, full-speed, super-speed */
	const unsigned int packet_sizes[3] = { 64, 512, 1024, };
	struct usb_endpoint_descriptor_no_audio *ep;
	struct usb_ss_ep_comp_descriptor *comp;
	struct usb_interface_descriptor *desc;
	struct usb_ffs_header *hdr;
	unsigned int i, pipe_id;

	hdr = calloc(1, size);
	if (!hdr) {
		errno = ENOMEM;
		return NULL;
	}

	hdr->header.magic = LE32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
	hdr->header.length = htole32(size);
	hdr->header.flags = LE32(FUNCTIONFS_HAS_FS_DESC |
				 FUNCTIONFS_HAS_HS_DESC |
				 FUNCTIONFS_HAS_SS_DESC);

	hdr->nb_fs = htole32(2);
	hdr->nb_hs = htole32(2);
	hdr->nb_ss = htole32(3);

	desc = ((void *) hdr) + sizeof(*hdr);

	for (i = 0; i < 3; i++) {
		desc->bLength = sizeof(*desc);
		desc->bDescriptorType = USB_DT_INTERFACE;
		desc->bInterfaceClass = USB_CLASS_COMM;
		desc->bNumEndpoints = 1;
		desc->iInterface = 1;

		ep = (struct usb_endpoint_descriptor_no_audio *)(desc + 1);

		ep->bLength = sizeof(*ep);
		ep->bDescriptorType = USB_DT_ENDPOINT;
		ep->bEndpointAddress = 1 | USB_DIR_OUT;
		ep->bmAttributes = USB_ENDPOINT_XFER_BULK;
		ep->wMaxPacketSize = htole16(packet_sizes[i]);

		if (i == 2) {
			comp = (struct usb_ss_ep_comp_descriptor *)(ep + 1);
			comp->bLength = USB_DT_SS_EP_COMP_SIZE;
			comp->bDescriptorType = USB_DT_SS_ENDPOINT_COMP;
		}

		desc = (void *)(desc + 1) + sizeof(*ep);
	}

	return hdr;
}

static int write_header(int fd)
{
	uint32_t size = sizeof(struct usb_ffs_header) +
		3 * sizeof(struct usb_interface_descriptor) +
		3 * sizeof(struct usb_endpoint_descriptor_no_audio) +
		1 * sizeof(struct usb_ss_ep_comp_descriptor);
	struct usb_ffs_header *hdr;
	int ret;

	hdr = create_header(size);
	if (!hdr)
		return -errno;

	ret = write(fd, hdr, size);
	free(hdr);
	if (ret < 0)
		return -errno;

	ret = write(fd, &ffs_strings, sizeof(ffs_strings));
	if (ret < 0)
		return -errno;

	return 0;
}

static void set_handler(int signal, void (*handler)(int))
{
	struct sigaction sig;

	sigaction(signal, NULL, &sig);
	sig.sa_handler = handler;
	sigaction(signal, &sig, NULL);
}

static void sig_handler(int sig)
{
	jzboot_exit();
}

int main(int argc, char **argv)
{
	int ret, ep0_fd, udc_fd;
	struct pdata pdata;
	char buf[256];

	if (argc < 4) {
		printf("Usage:\n\n    odbootd <ffs mountpoint> <UDC configfs file> <UDC name>\n");
		return EXIT_FAILURE;
	}

	snprintf(buf, sizeof(buf), "%s/ep0", argv[1]);
	ep0_fd = open(buf, O_RDWR);
	if (ep0_fd < 0) {
		ret = errno;
		printf("Unable to open ep0: %s\n", strerror(ret));
		return ret;
	}

	stop_fd = eventfd(0, EFD_NONBLOCK);
	if (stop_fd == -1) {
		ret = -errno;
		printf("Unable to create eventfd: %s\n", strerror(-ret));
		goto out_close;
	}

	set_handler(SIGHUP, sig_handler);
	set_handler(SIGPIPE, sig_handler);
	set_handler(SIGINT, sig_handler);
	set_handler(SIGTERM, sig_handler);

	ret = write_header(ep0_fd);
	if (ret < 0) {
		printf("Unable to write header: %s\n", strerror(-ret));
		goto out_close_eventfd;
	}

	snprintf(buf, sizeof(buf), "%s/ep1", argv[1]);
	pdata.ep1_fd = open(buf, O_RDONLY);
	if (pdata.ep1_fd < 0) {
		ret = -errno;
		printf("Unable to open ep1: %s\n", strerror(-ret));
		goto out_close_eventfd;
	}

	udc_fd = open(argv[2], O_WRONLY | O_TRUNC);
	if (udc_fd < 0) {
		ret = -errno;
		printf("Unable to open UDC: %s\n", strerror(-ret));
		goto out_close_ep1;
	}

	write(udc_fd, argv[3], strlen(argv[3]));
	close(udc_fd);

	for (;;) {
		struct usb_functionfs_event event;
		struct pollfd pfd[2];
		int ret;

		pfd[0].fd = ep0_fd;
		pfd[0].events = POLLIN;
		pfd[0].revents = 0;
		pfd[1].fd = stop_fd;
		pfd[1].events = POLLIN;
		pfd[1].revents = 0;

		poll_nointr(pfd, 2);

		if (pfd[1].revents & POLLIN) /* STOP event */
			break;

		if (pfd[0].revents & POLLIN) {
			read(ep0_fd, &event, sizeof(event));

			ret = handle_event(&pdata, &event);
			if (ret) {
				fprintf(stderr, "Unable to handle event: %s\n", strerror(-ret));
				break;
			}

			/* Clear out the errors on ep0 when we close endpoints */
			read(ep0_fd, NULL, 0);
		}
	}

out_close_ep1:
	close(pdata.ep1_fd);
out_close_eventfd:
	close(stop_fd);
out_close:
	close(ep0_fd);
	return -ret;
}
