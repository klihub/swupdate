/*
 * Copyright (c) 2017, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Intel Corporation nor the names of its contributors
 *     may be used to endorse or promote products derived from this software
 *     without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <libconfig.h>

#include "swupdate.h"
#include "handler.h"
#include "util.h"

#ifndef DATADIR
#	define DATADIR "/usr/share/"
#endif

#ifndef HOOKDIR
#	define HOOKDIR DATADIR"/swupdate/swupd-hooks.d"
#endif

#ifndef MAINHOOK
#	define MAINHOOK HOOKDIR"/run-hooks"
#endif

#ifndef MAX
#	define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif


/*
 * handler I/O-(well, just O)-proxying for hooks
 */
#define RD 0
#define WR 1
#define IO_BUF 4096
typedef struct {
	int  fd;                         /* child fd, sort array by this */
	int  pipe[2];                    /* pipe if proxied, -1 for /dev/null */
	int  level;                      /* message level */
	char buf[IO_BUF];                /* message collection buffer */
	int  len;                        /* buffer usage */
} io_proxy_t;


/*
 * configuration (passed in the environment to hooks)
 *
 * We pass on the supplied swupd configuratio (libconfig settings) to
 * the hooks in the environment. Apart from lists, we fully support
 * the libconfig set. A ridiculously overcomplicated configuration like
 * the following
 *
 *   swupd = {
 *      server_url = "http://update.example.org/swupd/";
 *      version = 213;
 *      env = {
 *          foo = "bar";
 *          foobar = "barfoo";
 *          one = 1;
 *          two = 2;
 *          pi = 3.141;
 *          an-array = [ 1, 2, 3, 4, 5 ];
 *            a-group = {
 *            another_array = [ "a", "b", "c", "d", "e", "f" ];
 *            string = "a string";
 *            integer = 12345;
 *            double = 9.81;
 *            boolean = true;
 *            negative-boolean = false;
 *          };
 *      };
 *   };
 *
 * is translated to the folowing environment:
 *
 *   SWUPD_SERVER_URL = 'http://update.example.org/swupd/'
 *   SWUPD_VERSION = '213'
 *   SWUPD_ENV_FOO = 'bar'
 *   SWUPD_ENV_FOOBAR = 'barfoo'
 *   SWUPD_ENV_ONE = '1'
 *   SWUPD_ENV_TWO = '2'
 *   SWUPD_ENV_PI = '3.141000'
 *   SWUPD_ENV_AN_ARRAY_0 = '1'
 *   SWUPD_ENV_AN_ARRAY_1 = '2'
 *   SWUPD_ENV_AN_ARRAY_2 = '3'
 *   SWUPD_ENV_AN_ARRAY_3 = '4'
 *   SWUPD_ENV_AN_ARRAY_4 = '5'
 *   SWUPD_ENV_A_GROUP_ANOTHER_ARRAY_0 = 'a'
 *   SWUPD_ENV_A_GROUP_ANOTHER_ARRAY_1 = 'b'
 *   SWUPD_ENV_A_GROUP_ANOTHER_ARRAY_2 = 'c'
 *   SWUPD_ENV_A_GROUP_ANOTHER_ARRAY_3 = 'd'
 *   SWUPD_ENV_A_GROUP_ANOTHER_ARRAY_4 = 'e'
 *   SWUPD_ENV_A_GROUP_ANOTHER_ARRAY_5 = 'f'
 *   SWUPD_ENV_A_GROUP_STRING = 'a string'
 *   SWUPD_ENV_A_GROUP_INTEGER = '12345'
 *   SWUPD_ENV_A_GROUP_DOUBLE = '9.810000'
 *   SWUPD_ENV_A_GROUP_BOOLEAN = 'TRUE'
 *   SWUPD_ENV_A_GROUP_NEGATIVE_BOOLEAN = 'FALSE'
 *
 * If we readily collected environment variables into an array of
 * environment strings (each of the format name=value) that could
 * be directly passed to execve avoiding some extra copying and
 * iterating over the variables. Can be easily changed...
 */
typedef struct {
	char *name;                      /* environment variabel name */
	char *value;                     /* and value */
} swupd_env_t;


/*
 * runtime context
 */
typedef struct {
	struct img_type *img;            /* 'swupdate image' we're installing */
	void            *data;           /* unused, our registered data */
	config_t         cfg;            /* our libconfig extracted from img */
	swupd_env_t      envv[128];      /* config as env. variables */
	int              envc;           /* number of env. variables */
} swupd_t;




static int swupd_mkenv(swupd_t *swupd);
static int swupd_setenv(swupd_t *swupd);


static void io_proxy_init(io_proxy_t *io, int fd, int level)
{
	io->level = level;
	io->len   = 0;
	io->fd    = fd;

	if (level != OFF)
		pipe(io->pipe);
	else
		io->pipe[RD] = io->pipe[WR] = -1;
}


static void io_proxy_child(io_proxy_t *io)
{
	int fd;

	/*
	 * set up proxying on the child side (io must be sorted by fd)
	 *
	 * loop through all fd's,
	 *   - close all non-proxied ones (ones without a matching ->fd)
	 *   - redirect proxied ones (dup to the pipe, -1 meaning /dev/null)
	 */
	
	for (fd = 0; fd < sysconf(_SC_OPEN_MAX); fd++) {
		close(fd);

		if (fd != io->fd)
			continue;
		
		if (io->level != OFF) {
			close(io->pipe[RD]);
			dup2(io->pipe[WR], io->fd);
			close(io->pipe[WR]);
		}
		else
			dup2(open("/dev/null", !io->fd ? O_WRONLY : O_RDONLY),
			     io->fd);

		io++;
	}
}


static void io_proxy_parent(io_proxy_t *io)
{
	int nonblk = O_NONBLOCK;

	/*
	 * set up proxying on the parent side
	 *
	 * loop through the proxied fd's,
	 *    - setting the read-size non-blocking
	 *    - closing the write (child) fd
	 *    - resetting the read buffer
	 */

	nonblk = O_NONBLOCK;
	for ( ; io->fd != -1; io++) {
		if (io->level == OFF)
			continue;

		fcntl(io->pipe[RD], F_SETFL, nonblk);
		close(io->pipe[WR]);
	}
}


static inline void io_proxy_write(int level, const char *s, int n)
{
	char msg[NOTIFY_BUF_SIZE];

	snprintf(msg, sizeof(msg), "[swupd] %.*s", n, s);

	if (level == ERRORLEVEL)
		notify(RUN, RECOVERY_NO_ERROR, msg);
	else
		notify(RUN, RECOVERY_NO_ERROR, msg);
}


static int io_proxy_drain(io_proxy_t *io)
{
	int   fd = io->pipe[RD];
	char *msg, *end;
	int   len, n;

	/*
	 * proxy pending messages (collecting data until '\n' or lack of space)
	 */

 retry:
	while ((n = read(fd, io->buf + io->len, IO_BUF - io->len)) > 0) {
		io->len += n;

		len = io->len;
		msg = io->buf;
		while ((end = memchr(msg, '\n', len))) {
			n = end - msg;
			io_proxy_write(io->level, msg, n);
			msg  = end + 1;
			len -= n + 1;
		}

		if (len == 0)
			io->len = 0;
		else {
			memmove(io->buf, io->buf + IO_BUF - len, len);
			io->len = len;
		}

		if (io->len == IO_BUF) {
			io_proxy_write(io->level, io->buf, io->len);
			io->len = 0;
		}
	}

	if (n < 0) {
		if (errno == EINTR)
			goto retry;

		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 1;
		
		ERROR("swupd: failed to proxy message (%d: %s)",
		      errno, strerror(errno));
		return -1;
	}

	if (n == 0) {
		close(io->pipe[RD]);
		io->pipe[RD] = -1;
	}
		
	return n > 0;
}


static inline int io_proxy_fdset(io_proxy_t *io, fd_set *rfds)
{
	int maxfd = -1, fd;

	FD_ZERO(rfds);
	
	for ( ; io->fd != -1; io++) {
		if (io->level == OFF)
			continue;

		if ((fd = io->pipe[RD]) == -1)
			continue;
		
		FD_SET(fd, rfds);
		maxfd = MAX(maxfd, fd);
	}

	return maxfd;
}


static int io_proxy_messages(io_proxy_t *io)
{
	io_proxy_t *p;
	int         maxfd, fd, n;
	fd_set      rfds;

	/*
	 * proxy all messages (until all fds are closed by child)
	 */

	while ((maxfd = io_proxy_fdset(io, &rfds)) > 0) {
		n = select(maxfd + 1, &rfds, NULL, NULL, NULL);

		if (n < 0) {
			ERROR("swupd: select failed (%d: %s)",
			      errno, strerror(errno));
			return -1;
		}

		for (p = io; p->fd != -1; p++) {
			if (p->level == OFF)
				continue;

			if ((fd = p->pipe[RD]) < 0)
				continue;

			if (FD_ISSET(fd, &rfds))
				io_proxy_drain(p);
		}
	}

	return maxfd;
		
}


static int swupd_run_hooks(swupd_t *swu)
{
	char       *argv[] = { (char *)MAINHOOK, NULL };
	io_proxy_t  io[16];
	pid_t       pid, w;
	int         status;

	io_proxy_init(io + 0, 0, OFF);
	io_proxy_init(io + 1, 1, INFOLEVEL);
	io_proxy_init(io + 2, 2, ERRORLEVEL);
	io_proxy_init(io + 3, 3, WARNLEVEL);
	io_proxy_init(io + 4, 4, DEBUGLEVEL);
	io_proxy_init(io + 5, 5, TRACELEVEL);
	io[6].fd = io[6].pipe[RD] = io[6].pipe[WR] = -1;
	
	switch ((pid = fork())) {
	case -1:
		ERROR("failed to fork()to exec '%s' (%d: %s)",
		      argv[0], errno, strerror(errno));
		return -1;

	case 0:
		io_proxy_child(io);

		if (swupd_mkenv(swu) < 0)
			return -1;
		else
			swupd_setenv(swu);

		if (execv(argv[0], argv) < 0) {
			ERROR("swupd-handler: failed to exec '%s' (%d: %s)",
			      argv[0], errno, strerror(errno));
			exit(-1);
		}
		break;

	default:
		io_proxy_parent(io);

		while (io_proxy_messages(io) > 0)
			;

		w = waitpid(pid, &status, WNOHANG);

		if (w < 0) {
			ERROR("swupd: failed to wait for child (%d: %s)",
			      errno, strerror(errno));
			return -1;
		}

		if (WIFSIGNALED(status)) {
			INFO("swupd: child exited with signal %d",
			     WTERMSIG(status));

			return -1;
		}

		if (WIFEXITED(status)) {
			status = WEXITSTATUS(status);
			INFO("swupd: child exited with status %d", status);

			return (status == 0 ? 0 : -1);
		}

		return -1;
	}

	return -1;
}


static void swupd_config_init(swupd_t *swu)
{
	config_init(&swu->cfg);
}


static void swupd_config_exit(swupd_t *swu)
{
	config_destroy(&swu->cfg);
}


static int copy_memcpy(void *out, const void *buf, int len)
{
	char **dstp = out;

	memcpy(*dstp, buf, len);
	*dstp += len;

	return 0;
}


static int swupd_config_read(swupd_t *swu)
{
	config_t *cfg        = &swu->cfg;
	struct img_type *img = swu->img;
	char buf[16 * 1024], *p;

	/*
	 * read our config content from image/cpio archive
	 */

	if (img->size > sizeof(buf) - 1) {
		ERROR("swupd: configuration file too big (%lld > %zd).",
		      img->size, sizeof(buf) - 1);
		return -1;
	}

	buf[0] = '\0';
	p = buf;
	if (copyfile(img->fdin, &p,
		     img->size, (unsigned long *)&img->offset,
		     0 /* no skip */, img->compressed,
		     &img->checksum, img->sha256, img->is_encrypted,
		     copy_memcpy) < 0) {
		ERROR("swupd: failed to read swupd configuration (%d: %s).",
		      errno, strerror(errno));
		return -1;
	}

	*p = '\0';

	TRACE("swupd: configuration: \"%s\"", buf);

	if (config_read_string(cfg, buf) != CONFIG_TRUE) {
		ERROR("swupd: failed to parse configuration (error: %s).",
		      config_error_text(cfg));
		return -1;
	}

	return 0;
}


static int swupd_config_check(swupd_t *swu)
{
	config_t *cfg = &swu->cfg;
	config_setting_t *swupd;

	/*
	 * perform absolute minimum, almost content-unaware config check
	 */

	swupd = config_lookup(cfg, "swupd");

	if (swupd == NULL)
		goto missing_swupd_node;

	if (config_setting_index(swupd) != 0 ||
	    config_setting_get_elem(config_root_setting(cfg), 1) != NULL)
		goto unhandled_nodes;

	return 0;

 missing_swupd_node:
	ERROR("swupd: malformed configuration, missing root swupd node.");
	return -1;

 unhandled_nodes:
	ERROR("swupd: malformed configuration, extra non-swupd nodes.");
	return -1;
}


static const char *envvar_value(char *buf, size_t size, config_setting_t *cs)
{
	int n;

	switch (config_setting_type(cs)) {
	case CONFIG_TYPE_STRING:
		return config_setting_get_string(cs);

	case CONFIG_TYPE_BOOL:
		return config_setting_get_bool(cs) ? "TRUE" : "FALSE";

	case CONFIG_TYPE_INT:
		n = snprintf(buf, size, "%d", config_setting_get_int(cs));
		break;

	case CONFIG_TYPE_INT64:
		n = snprintf(buf, size, "%lld", config_setting_get_int64(cs));
		break;

	case CONFIG_TYPE_FLOAT:
		n = snprintf(buf, size, "%f", config_setting_get_float(cs));
		break;

	default:
		goto invalid_type;
	}

	if (n >= (int)size)
		goto nospace;

	return buf;

 nospace:
	ERROR("swupd: can't set env var, value too long");
	return NULL;

 invalid_type:
	ERROR("swupd: can't set env var, invalid setting type (non-scalar).");
	return NULL;
}


static int setenv_scalar(swupd_env_t *e, const char *name, config_setting_t *cs)
{
	char vbuf[256], *p;
	const char *value;

	if ((value = envvar_value(vbuf, sizeof(vbuf), cs)) == NULL)
		return -1;

	e->name  = strdup(name);
	e->value = strdup(value);

	if (e->name == NULL || e->value == NULL)
		goto nomem;

	for (p = e->name; *p; p++) {
		if (isalpha(*p)) {
			*p = toupper(*p);
			continue;
		}

		if (isdigit(*p) || *p == '_')
			continue;

		else
			*p = '_';
	}

	TRACE("swupd: set env var '%s' = '%s'", e->name, e->value);

	return 0;

 nomem:
	ERROR("swupd: can't set env var, out of memory");
	return -1;
}


static int setenv_group(swupd_env_t *envv, int envc, const char *prfx,
			config_setting_t *cs);
static int setenv_array(swupd_env_t *envv, int envc, const char *prfx,
			config_setting_t *cs);

static int setenv_group(swupd_env_t *envv, int envc, const char *prfx,
			config_setting_t *grp)
{
	config_setting_t *cs;
	char              name[256];
	int               i, cnt, n;
	
	for (i = cnt = 0; (cs = config_setting_get_elem(grp, i)); i++) {
		if (cnt >= envc)
			return -1;

		snprintf(name, sizeof(name), "%s%s%s", prfx, *prfx ? "_" : "",
			 config_setting_name(cs));
		
		if (config_setting_is_scalar(cs)) {
			if (setenv_scalar(envv, name, cs) < 0)
				return -1;
			cnt++;
			envv++;
		}
		else if (config_setting_is_group(cs)) {
			n = setenv_group(envv, envc - cnt, name, cs);

			if (n < 0)
				return -1;

			cnt  += n;
			envv += n;
		}
		else if (config_setting_is_array(cs)) {
			n = setenv_array(envv, envc - cnt, name, cs);

			if (n < 0)
				return -1;

			cnt  += n;
			envv += n;
		}
		else
			goto invalid_type;
	}

	return cnt;
	
 invalid_type:
	ERROR("swupd: can't set envvar (%s), invalid setting type.", name);
	return -1;
}


static int setenv_array(swupd_env_t *envv, int envc, const char *prfx,
			config_setting_t *arr)
{
	config_setting_t *cs;
	char              name[256];
	int               i, cnt, n;
	
	for (i = cnt = 0; (cs = config_setting_get_elem(arr, i)); i++) {
		if (cnt >= envc)
			return -1;

		snprintf(name, sizeof(name), "%s_%d", prfx, i);
		
		if (config_setting_is_scalar(cs)) {
			if (setenv_scalar(envv, name, cs) < 0)
				return -1;
			cnt++;
			envv++;
		}
		else if (config_setting_is_group(cs)) {
			n = setenv_group(envv, envc - cnt, name, cs);

			if (n < 0)
				return -1;

			cnt  += n;
			envv += n;
		}
		else if (config_setting_is_array(cs)) {
			n = setenv_array(envv, envc - cnt, name, cs);

			if (n < 0)
				return -1;

			cnt  += n;
			envv += n;
		}
		else
			goto invalid_type;
	}

	return cnt;
	
 invalid_type:
	ERROR("swupd: can't set envvar (%s), invalid setting type.", name);
	return -1;
}


static int swupd_mkenv(swupd_t *swupd)
{
	int envc = sizeof(swupd->envv) / sizeof(swupd->envv[0]);

	envc = setenv_group(swupd->envv, envc, "",
			    config_root_setting(&swupd->cfg));

	if (envc < 0)
		return -1;

	swupd->envc = envc;
	
	return 0;
}


static int swupd_setenv(swupd_t *swupd)
{
	swupd_env_t *e = swupd->envv;

	for (; e->name != NULL; e++)
		setenv(e->name, e->value, 1);

	return 0;
}


static int swupd_init(swupd_t *swu, struct img_type *img, void *data)
{
	memset(swu, 0, sizeof(*swu));
	swu->img  = img;
	swu->data = data;

	swupd_config_init(swu);

	if (swupd_config_read(swu))
		return -1;

	if (swupd_config_check(swu))
		return -1;

	return 0;
}


static void swupd_exit(swupd_t *swu)
{
	swupd_config_exit(swu);
}


static int install_swupd_update(struct img_type *img, void *data)
{
	swupd_t swu;
	int     status;

	swupd_init(&swu, img, data);
	status = swupd_run_hooks(&swu);
	swupd_exit(&swu);

	return status;
}


__attribute__((constructor))
static void swupd_handler(void)
{
	register_handler("swupd", install_swupd_update, NULL);
}
