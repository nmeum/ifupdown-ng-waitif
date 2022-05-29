#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <net/if.h>
#include <libmnl/libmnl.h>
#include <linux/if.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>

#define LEN(X) (sizeof(X) / sizeof(X[0]))

typedef enum {
	PHASE_PRE_UP,
	PHASE_UP,
	PHASE_UNKNOWN,
} ifupdown_phase;

struct {
	const char *name;
	const ifupdown_phase value;
} phases[] = {
	{ "pre-up", PHASE_PRE_UP },
	{ "up", PHASE_UP },
};

struct context {
	int fd;
	int if_idx;
	const char *fifo_path;
};

/* TODO: Support $VERBOSE */

static ifupdown_phase
get_phase(void)
{
	const char *phase;

	if (!(phase = getenv("PHASE")))
		errx(EXIT_FAILURE, "Couldn't determine current phase");

	for (size_t i = 0; i < LEN(phases); i++) {
		if (!strcmp(phase, phases[i].name))
			return phases[i].value;
	}

	return PHASE_UNKNOWN;
}

static const char*
get_iface(void)
{
	const char *iface;

	if (!(iface = getenv("IFACE")))
		errx(EXIT_FAILURE, "Couldn't determine interface name");
	return iface;
}

static const char*
fifo_path(const char *iface)
{
	int ret;
	static char fp[PATH_MAX+1];

	ret = snprintf(fp, sizeof(fp), "%s/%s.waitif", RUNDIR, iface);
	if (ret < 0) {
		err(EXIT_FAILURE, "snprintf failed");
	} else if ((size_t)ret >= sizeof(fp)) {
		errx(EXIT_FAILURE, "snprintf: insufficient buffer size");
	}

	return fp;
}

static int
data_cb(const struct nlmsghdr *nlh, void *data)
{
	struct context *ctx;
	struct ifinfomsg *ifm;

	ctx = (struct context*)data;
	ifm = mnl_nlmsg_get_payload(nlh);

	if (ifm->ifi_index == ctx->if_idx && ifm->ifi_flags & IFF_RUNNING) {
		if ((ctx->fd = open(ctx->fifo_path, O_WRONLY)) == -1)
			return MNL_CB_ERROR;

		/* The proces running up() should be unblocked, we can stop. */
		return MNL_CB_STOP;
	}

	return MNL_CB_OK;
}

static int
wait_if(struct context *ctx)
{
	int ret;
	struct mnl_socket *nl;
	char buf[MNL_SOCKET_BUFFER_SIZE];

	nl = mnl_socket_open(NETLINK_ROUTE);
	if (nl == NULL)
		return -1;
	if (mnl_socket_bind(nl, RTMGRP_LINK, MNL_SOCKET_AUTOPID) < 0) {
		mnl_socket_close(nl);
		return -1;
	}

	ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
	while (ret > 0) {
		ret = mnl_cb_run(buf, ret, 0, 0, data_cb, (void *)ctx);
		if (ret <= 0)
			break;
		ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
	}
	if (ret == -1) {
		mnl_socket_close(nl);
		return -1;
	}

	mnl_socket_close(nl);
	return ctx->fd;
}

static void
pre_up(void)
{
	pid_t pid;
	const char *fp;
	const char *iface;
	struct context ctx;

	iface = get_iface();
	fp = fifo_path(iface);
	if (mkfifo(fp, 0600) == -1)
		err(EXIT_FAILURE, "mkfifo failed");

	ctx.fifo_path = fp;
	if (!(ctx.if_idx = if_nametoindex(iface)))
		errx(EXIT_FAILURE, "Unknown interface '%s'", iface);

	pid = fork();
	switch (pid) {
	case 0: {
		int fd;
		const char *msg;

		// XXX: If open(3) or write(3) fails, we can't do any
		// meaningful error handling as up() won't be unblocked.
		if ((fd = wait_if(&ctx)) == -1) {
			msg = strerror(errno);
			if ((fd = open(ctx.fifo_path, O_WRONLY)) == -1)
				err(EXIT_FAILURE, "open write-end failed");
			if (write(fd, msg, strlen(msg)) == -1 || write(fd, "\n", 1) == -1)
				err(EXIT_FAILURE, "write failed");
		}

		close(fd);
		break;
	}
	case -1:
		err(EXIT_FAILURE, "fork failed");
	}

	/* Parent just exits, child is kept running */
}

static void
up(void)
{
	int fd;
	ssize_t ret;
	const char *fp;
	const char *iface;
	char buf[BUFSIZ];

	/* TODO: Setup SIGALRM handler for timeout */

	iface = get_iface();
	fp = fifo_path(iface);

	/* Block until writing end of FIFO was opened,
	 * i.e. until the interface is up and running. */
	if ((fd = open(fp, O_RDONLY)) == -1)
		err(EXIT_FAILURE, "opening read-end failed");

	while ((ret = read(fd, buf, sizeof(buf))) > 0) {
		if (write(STDERR_FILENO, buf, ret) == -1)
			err(EXIT_FAILURE, "writing error message failed");
	}
	if (ret == -1)
		err(EXIT_FAILURE, "read failed");

	close(fd);
	if (unlink(fp) == -1)
		err(EXIT_FAILURE, "unlink failed");
}

int
main(void)
{
	switch (get_phase()) {
	case PHASE_PRE_UP:
		pre_up();
		return EXIT_SUCCESS;
	case PHASE_UP:
		up();
		return EXIT_SUCCESS;
	default:
		/* Don't need to do anything in this phase */
		return EXIT_SUCCESS;
	}
}
