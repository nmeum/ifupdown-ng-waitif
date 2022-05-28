#include <err.h>
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
		if (open(ctx->fifo_path, O_WRONLY) == -1)
			err(EXIT_FAILURE, "opening write-end failed");

		/* The proces running up() should be unblocked, we can stop. */
		return MNL_CB_STOP;
	}

	return MNL_CB_OK;
}

static void
wait_if(struct context *ctx)
{
	int ret;
	struct mnl_socket *nl;
	char buf[MNL_SOCKET_BUFFER_SIZE];

	nl = mnl_socket_open(NETLINK_ROUTE);
	if (nl == NULL)
		err(EXIT_FAILURE, "mnl_socket_open failed");
	if (mnl_socket_bind(nl, RTMGRP_LINK, MNL_SOCKET_AUTOPID) < 0)
		err(EXIT_FAILURE, "mnl_socket_bind failed");

	ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
	while (ret > 0) {
		ret = mnl_cb_run(buf, ret, 0, 0, data_cb, (void *)ctx);
		if (ret <= 0)
			break;
		ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
	}
	if (ret == -1)
		err(EXIT_FAILURE, "libmnl receive error");

	mnl_socket_close(nl);
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
	case 0:
		wait_if(&ctx);
		/* TODO: Need to unblock read-end if wait_if fails */
		break;
	case -1:
		err(EXIT_FAILURE, "fork failed");
	}

	/* Parent just exits, child is kept running */
}

static void
up(void)
{
	const char *fp;
	const char *iface;

	/* TODO: Setup SIGALRM handler for timeout */

	iface = get_iface();
	fp = fifo_path(iface);

	/* Block until writing end of FIFO was opened,
	 * i.e. until the interface is up and running. */
	if (open(fp, O_RDONLY) == -1)
		err(EXIT_FAILURE, "opening read-end failed");
}

int
main(void)
{
	/* TODO: Destroy FIFO in post-up. */
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
