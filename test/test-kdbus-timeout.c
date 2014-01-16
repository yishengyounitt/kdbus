#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <poll.h>
#include <sys/ioctl.h>

#include "kdbus-util.h"
#include "kdbus-enum.h"

static uint64_t expected = 0;

int timeout_msg_recv(struct conn *conn)
{
	uint64_t off;
	struct kdbus_msg *msg;
	int ret;

	ret = ioctl(conn->fd, KDBUS_CMD_MSG_RECV, &off);
	if (ret < 0) {
		fprintf(stderr, "error receiving message: %d (%m)\n", ret);
		return EXIT_FAILURE;
	}

	msg = (struct kdbus_msg *)(conn->buf + off);
	msg_dump(conn, msg);
	expected &= ~(1ULL << msg->cookie_reply);
	printf("Got message timeout for cookie %llu\n", msg->cookie_reply);

	ret = ioctl(conn->fd, KDBUS_CMD_FREE, &off);
	if (ret < 0) {
		fprintf(stderr, "error free message: %d (%m)\n", ret);
		return EXIT_FAILURE;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	struct {
		struct kdbus_cmd_make head;

		/* bloom size item */
		struct {
			uint64_t size;
			uint64_t type;
			uint64_t bloom_size;
		} bs;

		/* name item */
		uint64_t n_size;
		uint64_t n_type;
		char name[64];
	} bus_make;
	int fdc, ret;
	char *bus;
	struct conn *conn_a, *conn_b;
	struct pollfd fds[1];
	int i, n_msgs = 4;

	printf("-- opening /dev/" KBUILD_MODNAME "/control\n");
	fdc = open("/dev/" KBUILD_MODNAME "/control", O_RDWR|O_CLOEXEC);
	if (fdc < 0) {
		fprintf(stderr, "--- error %d (%m)\n", fdc);
		return EXIT_FAILURE;
	}

	memset(&bus_make, 0, sizeof(bus_make));
	bus_make.head.flags = KDBUS_MAKE_POLICY_OPEN;
	bus_make.bs.size = sizeof(bus_make.bs);
	bus_make.bs.type = KDBUS_ITEM_BLOOM_SIZE;
	bus_make.bs.bloom_size = 64;

	snprintf(bus_make.name, sizeof(bus_make.name), "%u-testbus", getuid());
	bus_make.n_type = KDBUS_ITEM_MAKE_NAME;
	bus_make.n_size = KDBUS_ITEM_HEADER_SIZE + strlen(bus_make.name) + 1;

	bus_make.head.size = sizeof(struct kdbus_cmd_make) +
			     sizeof(bus_make.bs) +
			     bus_make.n_size;

	printf("-- creating bus '%s'\n", bus_make.name);
	ret = ioctl(fdc, KDBUS_CMD_BUS_MAKE, &bus_make);
	if (ret) {
		fprintf(stderr, "--- error %d (%m)\n", ret);
		return EXIT_FAILURE;
	}

	if (asprintf(&bus, "/dev/" KBUILD_MODNAME "/%s/bus", bus_make.name) < 0)
		return EXIT_FAILURE;

	conn_a = connect_to_bus(bus, 0);
	conn_b = connect_to_bus(bus, 0);
	if (!conn_a || !conn_b)
		return EXIT_FAILURE;

	fds[0].fd = conn_b->fd;

	/* send messages that expect a reply (within 1 sec), but never answer it */
	for (i = 0; i < n_msgs; i++) {
		printf("Sending message with cookie %u ...\n", i);
		msg_send(conn_b, NULL, i, KDBUS_MSG_FLAGS_EXPECT_REPLY, (i + 1) * 100ULL * 1000ULL, conn_a->id);
		expected |= 1ULL << i;
	}

	for (;;) {
		int nfds = sizeof(fds) / sizeof(fds[0]);

		for (i = 0; i < nfds; i++) {
			fds[i].events = POLLIN | POLLPRI | POLLHUP;
			fds[i].revents = 0;
		}

		printf("--- entering poll\n");
		ret = poll(fds, nfds, (n_msgs + 1) * 100);
		if (ret == 0)
			printf("--- timeout\n");
		if (ret <= 0)
			break;

		if (fds[0].revents & POLLIN)
			timeout_msg_recv(conn_b);

		if (expected == 0)
			break;
	}

	if (expected != 0) {
		for (i = 0; i < 64; i++)
			if (expected & (1ULL << i))
				printf("No timeout notification received for cookie %u\n", i);
	} else {
		printf("Timeout notifications received for all messages. Good.\n");
	}

	close(conn_a->fd);
	close(conn_b->fd);
	free(conn_a);
	free(conn_b);
	close(fdc);
	free(bus);

	return expected ? EXIT_FAILURE : EXIT_SUCCESS;
}