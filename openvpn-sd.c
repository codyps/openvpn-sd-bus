#include <ev.h>
#include <systemd/sd-bus.h>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct line_reader {
	char buf[1024];
	size_t pos;
};

static ssize_t
feed(struct line_reader *restrict lr, char *restrict d, size_t l)
{
	if ((l + lr->pos) > sizeof(lr->buf)) {
		return -ENOMEM;
	}

	memcpy(lr->buf + lr->pos, d, l);
	char *end = memchr(lr->buf + lr->pos, '\n', l);
	lr->pos += l;

	if (end)
		return end - lr->buf;
	return 0;
}

static void
eat(struct line_reader *restrict lr, size_t b)
{
	assert(b <= lr->pos);
	size_t new = lr->pos - b;
	memmove(lr->buf, lr->buf + b, new);
	lr->pos = new;
}

static int
method_connect(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	(void)userdata;
	(void)ret_error;
	printf("CONNECT!\n");
        return sd_bus_reply_method_return(m, "");
}

static int
method_disconnect(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	(void)userdata;
	(void)ret_error;
	printf("DISCONNECT!\n");
        return sd_bus_reply_method_return(m, "");
}

static const
sd_bus_vtable openvpn_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("Connect", "", "", method_connect, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Disconnect", "", "", method_disconnect, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END
};

struct ev__sdbus {
	ev_io w;
	sd_bus *bus;
};

struct ev__manage {
	ev_io w;
	struct line_reader lr;
};

static void
manage_cb(EV_P_ ev_io *w, int revents)
{
	(void)EV_A;
	struct ev__manage *s = (struct ev__manage *)w;
	(void)revents;

	ssize_t r = read(w->fd, s->lr.buf + s->lr.pos, sizeof(s->lr.buf) - s->lr.pos);
	if (r < 0) {
		fprintf(stderr, "read of manage socket failed: %s (%d)\n",
				strerror(errno), errno);
		exit(EXIT_FAILURE);
	}

	char *end = memchr(s->lr.buf + s->lr.pos, '\n', r);
	s->lr.pos += r;

	if (end) {
		/* handle line */
		printf("LINE: %.*s\n",
				(int)(end - s->lr.buf), s->lr.buf);
		eat(&s->lr, end - s->lr.buf + 1);
	}
	
	if (s->lr.pos)
		fprintf(stderr, "buffer contains %zu unused bytes\n",
				s->lr.pos);

	if (s->lr.pos == sizeof(s->lr.buf)) {
		fprintf(stderr, "error: overfull buffer, we got a line longer than %zu bytes\n",
				sizeof(s->lr.buf));
		exit(EXIT_FAILURE);
	}
}

static void
bus_cb(EV_P_ ev_io *w, int revents)
{
	(void)EV_A;
	struct ev__sdbus *s = (struct ev__sdbus *)w;
	(void)revents;
	/* Process requests */
	int r = 1;
	while (r) {
		r = sd_bus_process(s->bus, NULL);
		if (r < 0) {
			fprintf(stderr, "Failed to process bus: %s\n", strerror(-r));
			ev_break(EV_A_ EVBREAK_ALL);
		}
	}
}

static const char *opts = "hl:p:";

#define streq(a, b) (!strcmp(a, b))

#define usage(e) _usage(e, argc ? argv[0] : "openvpn-sd")
static void
_usage(int e, const char *prgm_name)
{
	FILE *f = e ? stdout : stderr;
	fprintf(f,
"usage: %s [options]\n"
"options: %s\n"
" -l <managment-host>\n"
" -p <port>\n"
"\n"
"If <port> is 'unix', then <managment-host> is used as the path to the unix-socket",
	prgm_name, opts);
	exit(e);
}

int
main(int argc, char *argv[])
{
	unsigned err = 0;
	int opt;
	const char *host = "localhost";
	const char *serv = NULL;

	while ((opt = getopt(argc, argv, opts)) != -1) {
		switch(opt) {
		case 'h':
			usage(0);
		case 'l':
			host = optarg;
			break;
		case 'p':
			serv = optarg;
			break;
		default:
			fprintf(stderr, "unknown opt '%c'\n", optopt);
			err++;
		}
	}

	if (!serv) {
		fprintf(stderr, "A value for '-p' is required, but was not provided\n");
		err++;
	}

	if (err) {
		fprintf(stderr, "Fatal error%s, exiting\n", (err > 1) ? "s" : "");
		exit(EXIT_FAILURE);
	}

	int sfd;
	/* TODO: can we do this async? what happens to us when we loose
	 * the connection */
	if (streq("unix", serv)) {
		/* create a unix socket */
		sfd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (sfd == -1) {
			fprintf(stderr, "Could not create unix socket: %s (%d)\n",
					strerror(errno), errno);
			exit(EXIT_FAILURE);
		}

		struct sockaddr_un a = {
			.sun_family = AF_UNIX,
		};
		/* TODO: limit copy length */
		size_t l = strlen(host) + 1;
		if (l > sizeof(a.sun_path)) {
			fprintf(stderr, "Unix path too long (was %zu bytes, max is %zu bytes)\n", l, sizeof(a.sun_path));
			exit(EXIT_FAILURE);
		}
		memcpy(a.sun_path, host, l);

		if (connect(sfd, (struct sockaddr *)&a, sizeof(a)) != 0) {
			fprintf(stderr, "Could not connect to unix socket '%s': %s (%d)\n",
					host, strerror(errno), errno);
			exit(EXIT_FAILURE);
		}
	} else {
		/* network socket */
		struct addrinfo hints = {
			.ai_family = AF_UNSPEC,
			.ai_socktype = SOCK_STREAM,
			.ai_flags = AI_ADDRCONFIG
		};
		struct addrinfo *res;
		int e = getaddrinfo(host, serv, &hints, &res);
		if (e) {
			fprintf(stderr, "Could not resolve host '%s' '%s'\n", host, serv);
			exit(EXIT_FAILURE);
		}

		struct addrinfo *a = res;
		while (a) {
			/* try to connect, on success done */
			/* TODO: impliment HappyEyes (or some sort of other
			 * connection algorithm?) */
			a = res->ai_next;

			sfd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
			if (sfd == -1)
				continue;

			if (connect(sfd, a->ai_addr, a->ai_addrlen) == 0)
				break;

			close(sfd);
		}
		if (!a) {
			/* TODO: show the actual errors we recieved on each of
			 * the ai_addrs */
			fprintf(stderr, "Could not connect to any hosts of '%s' '%s'\n", host, serv);
			exit(EXIT_FAILURE);
		} else {
			/* TODO: debug log the actual addr we used */
		}
		freeaddrinfo(res);
	}

	sd_bus_slot *slot = NULL;
	sd_bus *bus = NULL;
	int r;

	/* Connect to the user bus this time */
	r = sd_bus_open_user(&bus);
	if (r < 0) {
		fprintf(stderr, "Failed to connect to system bus: %s\n", strerror(-r));
		goto finish;
	}

	/* Install the object */
	r = sd_bus_add_object_vtable(bus,
			&slot,
			"/com/codyps/OpenVpn",  /* object path */
			"com.codyps.OpenVpn",   /* interface name */
			openvpn_vtable,
			NULL);
	if (r < 0) {
		fprintf(stderr, "Failed to issue method call: %s\n", strerror(-r));
		goto finish;
	}

	/* Take a well-known service name so that clients can find us */
	r = sd_bus_request_name(bus, "com.codyps.OpenVpn", 0);
	if (r < 0) {
		fprintf(stderr, "Failed to acquire service name: %s\n", strerror(-r));
		goto finish;
	}


	struct ev__sdbus sd_watcher;
	sd_watcher.bus = bus;
	ev_io_init(&sd_watcher.w, bus_cb, sd_bus_get_fd(bus), EV_READ);

	struct ev__manage manage_watcher = {};
	ev_io_init(&manage_watcher.w, manage_cb, sfd, EV_READ);

	ev_io_start(EV_DEFAULT_ &sd_watcher.w);
	ev_io_start(EV_DEFAULT_ &manage_watcher.w);

	ev_run(EV_DEFAULT_ 0);

finish:
	/* TODO: clean up other things */
	sd_bus_slot_unref(slot);
	sd_bus_unref(bus);

	return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
