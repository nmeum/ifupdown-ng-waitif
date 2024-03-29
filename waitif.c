#include <err.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <semaphore.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#include <net/if.h>
#include <libmnl/libmnl.h>
#include <linux/if.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>

/* Timeout after 30s by default, set to zero to wait indefinitely. */
#define DEFAULT_TIMEOUT 30

struct context {
	struct mnl_socket *nl;
	unsigned int if_idx;
	sem_t *sema;
};

static bool
get_timeout(unsigned int *r)
{
	unsigned long delay;
	const char *timeout;

	if (!(timeout = getenv("IF_WAITIF_TIMEOUT"))) {
		*r = DEFAULT_TIMEOUT; // no timeout configured
		return true;
	}

	errno = 0;
	delay = strtoul(timeout, NULL, 10);
	if (!delay && errno) {
		return false;
	} else if (delay > UINT_MAX) {
		errno = EOVERFLOW;
		return false;
	}

	*r = (unsigned int)delay;
	return true;
}

static int
data_cb(const struct nlmsghdr *nlh, void *arg)
{
	struct ifinfomsg *ifm;
	struct context *ctx;

	ctx = (struct context *)arg;
	ifm = mnl_nlmsg_get_payload(nlh);

	if ((unsigned)ifm->ifi_index == ctx->if_idx && ifm->ifi_flags & IFF_RUNNING)
		return MNL_CB_STOP;

	return MNL_CB_OK;
}

static void *
netlink_loop(void *arg)
{
	ssize_t ret;
	struct context *ctx;
	char buf[MNL_SOCKET_BUFFER_SIZE];

	ctx = (struct context*)arg;
	ret = mnl_socket_recvfrom(ctx->nl, buf, sizeof(buf));
	while (ret > 0) {
		ret = mnl_cb_run(buf, (size_t)ret, 0, 0, data_cb, arg);
		if (ret <= 0)
			break;
		ret = mnl_socket_recvfrom(ctx->nl, buf, sizeof(buf));
	}
	if (ret == -1) {
		warn("netlink_loop failed");
		return NULL;
	}

	sem_post(ctx->sema);
	return NULL;
}

static int
iface_is_up(struct mnl_socket *nl, const char *iface)
{
	int fd;
	size_t namelen;
	struct ifreq req;

	namelen = strlen(iface);
	if (namelen >= IFNAMSIZ) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(req.ifr_name, iface, namelen+1);

	fd = mnl_socket_get_fd(nl);
	if (ioctl(fd, SIOCGIFFLAGS, &req) < 0)
		return -1;
	return req.ifr_flags & IFF_RUNNING;
}

static bool
run_nl_thread(pthread_t *thread, sem_t *sema)
{
	static struct context ctx;
	const char *iface;
	int iface_state_up;

	if (!(iface = getenv("IFACE")) || !(ctx.if_idx = if_nametoindex(iface))) {
		errno = EINVAL;
		return false;
	}

	ctx.nl = mnl_socket_open(NETLINK_ROUTE);
	if (ctx.nl == NULL)
		return false;
	if (mnl_socket_bind(ctx.nl, RTMGRP_LINK, MNL_SOCKET_AUTOPID) == -1)
		return false;

	iface_state_up = iface_is_up(ctx.nl, iface);
	if (iface_state_up == -1)
		return false;

	// Check if the link was up prior to socket creation.
	if (iface_state_up) {
		mnl_socket_close(ctx.nl);
		sem_post(sema);
	} else {
		ctx.sema = sema;
		if ((errno = pthread_create(thread, NULL, netlink_loop, &ctx)))
			return false;
	}

	return true;
}

static bool
wait_for_iface(sem_t *sema, unsigned int timeout)
{
	struct timespec ts;

	// No timeout → block indefinitely
	if (!timeout) {
		if (sem_wait(sema) == -1)
			return false;
		return true;
	}

	if (clock_gettime(CLOCK_REALTIME, &ts))
		return false;
	ts.tv_sec += timeout;
	if (sem_timedwait(sema, &ts) == -1)
		return false; // detect timeout via errno

	return true;
}

int
main(void)
{
	sem_t sema;
	pthread_t thread;
	const char *phase;
	unsigned int timeout;

	// XXX: The executor doesn't require root privileges but is
	// started as root by ifupdown-ng. We could drop privileges here.

	// Executor only runs in "up" phase.
	if (!(phase = getenv("PHASE")))
		errx(EXIT_FAILURE, "Couldn't determine current phase");
	if (strcmp(phase, "up"))
		return EXIT_SUCCESS;

	if (!get_timeout(&timeout))
		err(EXIT_FAILURE, "get_timeout failed");

	if (getenv("VERBOSE")) {
		fprintf(stderr, "waitif: Waiting ");
		if (timeout)
			fprintf(stderr, "up to %u seconds", timeout);
		else
			fprintf(stderr, "indefinitely");
		fprintf(stderr, " for interface to come up\n");
	}

	sem_init(&sema, 0, 0);
	if (!run_nl_thread(&thread, &sema))
		err(EXIT_FAILURE, "run_nl_thread failed");
	if (!wait_for_iface(&sema, timeout))
		err(EXIT_FAILURE, "wait_for_iface failed");

	return EXIT_SUCCESS;
}
