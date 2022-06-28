#include <err.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
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

#define LEN(X) (sizeof(X) / sizeof(X[0]))

struct context {
	struct mnl_socket *nl;
	unsigned int if_idx;
	sem_t *sema;
};

static bool verbose;

#if 0
static void
debug(const char *restrict fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;

	va_start(ap, fmt);
	vwarnx(fmt, ap);
	va_end(ap);
}
#endif

static unsigned int
get_timeout(void)
{
	unsigned long delay;
	const char *timeout;

	if (!(timeout = getenv("IF_WAITIF_TIMEOUT")))
		return 0; // no timeout configured

	errno = 0;
	delay = strtoul(timeout, NULL, 10);
	if (!delay && errno)
		err(EXIT_FAILURE, "strtoul failed");
	else if (delay > UINT_MAX)
		errx(EXIT_FAILURE, "timeout value '%lu' exceeds UINT_MAX", delay);

	return (unsigned int)delay;
}

static const char*
get_iface(void)
{
	const char *iface;

	if (!(iface = getenv("IFACE")))
		errx(EXIT_FAILURE, "Couldn't determine interface name");
	return iface;
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
		fprintf(stderr, "ifupdown: netlink_loop failed %s\n", strerror(errno));
		return NULL;
	}

	// Success: Increment semaphore.
	if (sem_post(ctx->sema) == -1)
		err(EXIT_FAILURE, "sem_post failed");

	return NULL;
}

static bool
iface_is_up(struct mnl_socket *nl, const char *iface)
{
	int fd;
	size_t namelen;
	struct ifreq req;

	namelen = strlen(iface);
	if (namelen >= IFNAMSIZ)
		errx(EXIT_FAILURE, "interface name too long");
	memcpy(req.ifr_name, iface, namelen+1);

	fd = mnl_socket_get_fd(nl);
	if (ioctl(fd, SIOCGIFFLAGS, &req) < 0)
		err(EXIT_FAILURE, "ioctl with SIOCGIFFLAGS failed");
	return req.ifr_flags & IFF_RUNNING;
}

static void
start_nl_thread(pthread_t *thread, sem_t *sema)
{
	const char *iface;
	struct context ctx;

	iface = get_iface();
	if (!(ctx.if_idx = if_nametoindex(iface)))
		errx(EXIT_FAILURE, "Unknown interface '%s'", iface);

	ctx.nl = mnl_socket_open(NETLINK_ROUTE);
	if (ctx.nl == NULL)
		errx(EXIT_FAILURE, "mnl_socket_open failed");
	if (mnl_socket_bind(ctx.nl, RTMGRP_LINK, MNL_SOCKET_AUTOPID) < 0)
		errx(EXIT_FAILURE, "mnl_socket_bind failed");

	// Check if the link was up prior to socket creation.
	if (iface_is_up(ctx.nl, iface)) {
		mnl_socket_close(ctx.nl);
		if (sem_post(sema) == -1)
			err(EXIT_FAILURE, "sem_post failed");
	} else {
		ctx.sema = sema;
		if (pthread_create(thread, NULL, netlink_loop, &ctx))
			errx(EXIT_FAILURE, "pthread_create failed");
	}
}

int
main(void)
{
	sem_t sema;
	pthread_t thread;
	const char *phase;
	unsigned int timeout;

	// Set global verbose flag for debug function.
	verbose = getenv("VERBOSE") != NULL;

	// Executor only runs in "up" phase.
	if (!(phase = getenv("PHASE")))
		errx(EXIT_FAILURE, "Couldn't determine current phase");
	if (strcmp(phase, "up"))
		return EXIT_SUCCESS;

	timeout = get_timeout();
	start_nl_thread(&thread, &sema);

	if (timeout) {
		struct timespec ts;

		if (clock_gettime(CLOCK_REALTIME, &ts))
			err(EXIT_FAILURE, "clock_gettime failed");
		ts.tv_sec += timeout;

		if (sem_timedwait(&sema, &ts) == -1)
			err(EXIT_FAILURE, "sem_timedwait failed");
	} else {
		if (sem_wait(&sema) == -1)
			err(EXIT_FAILURE, "sem_wait failed");
	}
}
