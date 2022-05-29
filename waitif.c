#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
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
	unsigned int if_idx;
	sigset_t blockset;
	const char *fifo_path;
};

static bool verbose;

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

	if ((unsigned)ifm->ifi_index == ctx->if_idx && ifm->ifi_flags & IFF_RUNNING) {
		if (sigprocmask(SIG_BLOCK, &ctx->blockset, NULL))
			return MNL_CB_ERROR;
		if ((ctx->fd = open(ctx->fifo_path, O_WRONLY)) == -1)
			return MNL_CB_ERROR;

		// The proces running up() should be unblocked, we can stop.
		return MNL_CB_STOP;
	}

	return MNL_CB_OK;
}

static int
wait_if(struct context *ctx)
{
	ssize_t ret;
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
		ret = mnl_cb_run(buf, (size_t)ret, 0, 0, data_cb, (void *)ctx);
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
sigalarm(int num)
{
	int fd;
	const char *fp;
	const char *iface;
	const char *errmsg = "timeout\n";

	(void)num;

	iface = get_iface();
	fp = fifo_path(iface);

	if ((fd = open(fp, O_WRONLY)) == -1)
		err(EXIT_FAILURE, "open write-end failed");
	if (write(fd, errmsg, strlen(errmsg)) == -1)
		err(EXIT_FAILURE, "write failed");

	exit(EXIT_FAILURE);
}

static void
sethandler(void)
{
	struct sigaction act;

	act.sa_flags = SA_RESTART;
	act.sa_handler = sigalarm;
	if (sigemptyset(&act.sa_mask) == -1)
		err(EXIT_FAILURE, "sigemptyset failed");
	if (sigaction(SIGALRM, &act, NULL))
		err(EXIT_FAILURE, "sigaction failed");
}

static void
pre_up(void)
{
	pid_t pid;
	const char *iface;
	unsigned int delay;
	struct context ctx;

	iface = get_iface();
	if (!(ctx.if_idx = if_nametoindex(iface)))
		errx(EXIT_FAILURE, "Unknown interface '%s'", iface);
	debug("Starting watchdog for interface '%s' (index: %u)", iface, ctx.if_idx);

	sethandler();
	if (sigemptyset(&ctx.blockset) == -1)
		err(EXIT_FAILURE, "sigemptyset failed");
	sigaddset(&ctx.blockset, SIGALRM);
	if ((delay = get_timeout()))
		debug("Watchdog will timeout after %u seconds", delay);

	ctx.fifo_path = fifo_path(iface);
	if (mkfifo(ctx.fifo_path, 0600) == -1)
		err(EXIT_FAILURE, "mkfifo failed");
	debug("Created named pipe at: %s", ctx.fifo_path);

	pid = fork();
	switch (pid) {
	case 0: {
		int fd;
		const char *msg;

		// Setup timeout mechanism via alarm(3).
		if (delay) alarm(delay);

		// XXX: If open(3) or write(3) fails, we can't do any
		// meaningful error handling as up() won't be unblocked.
		if ((fd = wait_if(&ctx)) == -1) {
			msg = strerror(errno);

			// Ignore sigprocmask error as we always want
			// to unblock the up() process via open(3).
			sigprocmask(SIG_BLOCK, &ctx.blockset, NULL);

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
	default:
		debug("Watchdog child process spawned with PID: %ld", (long)pid);
	}

	// Parent just exits, child is kept running
}

static void
up(void)
{
	ssize_t ret;
	int fd, status;
	const char *fp;
	const char *iface;
	char buf[BUFSIZ];

	iface = get_iface();
	fp = fifo_path(iface);

	// Block until writing end of FIFO was opened,
	// i.e. until the interface is up and running.
	debug("Waiting for interface '%s' to switch to IFF_RUNNING", iface);
	if ((fd = open(fp, O_RDONLY)) == -1)
		err(EXIT_FAILURE, "opening read-end failed");

	status = EXIT_SUCCESS;
	while ((ret = read(fd, buf, sizeof(buf))) > 0) {
		// Received error message via FIFO, update status accordingly.
		status = EXIT_FAILURE;

		// TODO: Prefix error message with program name.
		// Ideally by buffering the message and using warnx(3).
		if (write(STDERR_FILENO, buf, (size_t)ret) == -1)
			err(EXIT_FAILURE, "writing error message failed");
	}
	if (ret == -1)
		err(EXIT_FAILURE, "read failed");
	debug("Watchdog for interface '%s' %s", iface, (status == EXIT_SUCCESS) ? "succeeded" : "failed");

	close(fd);
	debug("Removing named pipe at: %s", fp);
	if (unlink(fp) == -1)
		warn("unlink failed");
	exit(status);
}

int
main(void)
{
	// Set global verbose flag for debug function.
	verbose = getenv("VERBOSE") != NULL;

	switch (get_phase()) {
	case PHASE_PRE_UP:
		pre_up();
		return EXIT_SUCCESS;
	case PHASE_UP:
		up();
		return EXIT_SUCCESS;
	default:
		// Don't need to do anything in this phase
		return EXIT_SUCCESS;
	}
}
