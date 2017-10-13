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
#include <dirent.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <libconfig.h>

#include "generated/autoconf.h"
#include "swupdate.h"
#include "handler.h"
#include "util.h"

#define HOOKDIR_OVERRIDE "SWUPDATE_WRAPPER_HOOKDIR"

#ifndef DATADIR
#	define DATADIR "/usr/share"
#endif

#ifndef HOOKDIR
#	define HOOKDIR DATADIR"/swupdate/hooks.d"
#endif

#ifndef MAINHOOK
#	define MAINHOOK HOOKDIR"/run-hooks"
#endif

#ifndef MAX
#	define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef PATH_MAX
#       define PATH_MAX 1024
#endif

/*
 * handler I/O-(well, just O)-proxying for hooks
 */
#define RD 0
#define WR 1
#define IO_BUF 4096
#define CR_FORCE_WRITE 5
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
 * We pass on the supplied configuration (libconfig settings) to the
 * backend-specific hooks in the environment. Apart from lists, we fully
 * support the libconfig set. A ridiculously overcomplicated configuration
 * like the following
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
} wrap_env_t;


/*
 * runtime context
 */
typedef struct {
	const char      *hookdir;        /* hook directory */
	const char      *mainhook;       /* main hook */
	const char      *type;           /* wrapped type, e.g. "swupd" */
	struct img_type *img;            /* 'swupdate image' we're installing */
	void            *data;           /* unused, our registered data */
	config_t         cfg;            /* our libconfig extracted from img */
	wrap_env_t       envv[128];      /* config as env. variables */
	int              envc;           /* number of env. variables */
} wrap_t;


static int wrapper_mkenv(wrap_t *wrap);
static int wrapper_setenv(wrap_t *wrap);


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

	(void)level;

	if (n <= 0)
		return;

	if (s[n - 1] == '\r')
		n--;

	snprintf(msg, sizeof(msg), "[wrapper] %.*s", n, s);

	notify(RUN, RECOVERY_NO_ERROR, msg);
}


static int io_proxy_drain(io_proxy_t *io)
{
	int   fd = io->pipe[RD];
	char *msg, *end, *cr;
	int   len, n, crs;

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

		if (msg > io->buf && len > 0)
			memmove(io->buf, io->buf + IO_BUF - len, len);

		io->len = len;

		/*
		 * hack to show progress printouts
		 *
		 * Simple textual progress indicators are often
		 * implemented by outputting a progress message
		 * followed by a CR, often printing only when the
		 * quantity (percentage, counter, etc.) changes.
		 *
		 * Without special treatment all the messages for
		 * most of such progress indicators would fit into
		 * our output collection buffer (4K), hence we'd
		 * up printing out only the final one of the progress
		 * messages (typically 'done', 'ok', or 'success').
		 *
		 * As a compromise to print some progress, if we
		 * have a partial buffer without a trailing LF,
		 * we count the pending CR's and force a message write
		 * if there are more than CR_FORCE_WRITE pending.
		 */

		if (io->len > 0 && io->buf[io->len - 1] == '\r') {
			cr  = io->buf;
			len = io->len;
			crs = 0;

			while ((end = memchr(cr, '\r', len)) != NULL) {
				crs++;
				msg = cr;
				n   = end - msg + 1;
				len -= n;
				cr   = len > 0 ? end + 1 : NULL;
			}

			if (crs > CR_FORCE_WRITE) {
				io_proxy_write(io->level, msg, n);
				memmove(io->buf, msg + len,
					n = io->len - (msg + len - io->buf));
				io->len = n;
			}
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

		ERROR("wrapper: failed to proxy message (%d: %s)",
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
			ERROR("wrapper: select failed (%d: %s)",
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


static int wrapper_run_hooks(wrap_t *swu)
{
	char       *argv[] = { (char *)swu->mainhook, (char *)swu->type, NULL };
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
		ERROR("failed to fork() to exec '%s' (%d: %s)",
		      argv[0], errno, strerror(errno));
		return -1;

	case 0:
		io_proxy_child(io);

		if (wrapper_mkenv(swu) < 0)
			return -1;
		else
			wrapper_setenv(swu);

		if (execv(argv[0], argv) < 0) {
			ERROR("wrapper-handler: failed to exec '%s' (%d: %s)",
			      argv[0], errno, strerror(errno));
			_exit(-1);
		}
		break;

	default:
		io_proxy_parent(io);

		while (io_proxy_messages(io) > 0)
			;

		w = waitpid(pid, &status, 0);

		if (w < 0) {
			ERROR("wrapper: failed to wait for child (%d: %s)",
			      errno, strerror(errno));
			return -1;
		}

		if (WIFSIGNALED(status)) {
			INFO("wrapper: child exited with signal %d",
			     WTERMSIG(status));

			return -1;
		}

		if (WIFEXITED(status)) {
			status = WEXITSTATUS(status);
			INFO("wrapper: child exited with status %d", status);

			return (status == 0 ? 0 : -1);
		}

		return -1;
	}

	return -1;
}


static void wrapper_config_init(wrap_t *swu)
{
	config_init(&swu->cfg);
}


static void wrapper_config_exit(wrap_t *swu)
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


static int wrapper_config_read(wrap_t *swu)
{
	config_t *cfg        = &swu->cfg;
	struct img_type *img = swu->img;
	char buf[16 * 1024], *p;

	/*
	 * read our config content from image/cpio archive
	 */

	if (img->size > sizeof(buf) - 1) {
		ERROR("wrapper: configuration file too big (%lld > %zd).",
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
		ERROR("wrapper: failed to read configuration (%d: %s).",
		      errno, strerror(errno));
		return -1;
	}

	*p = '\0';

	TRACE("wrapper: configuration: \"%s\"", buf);

	if (config_read_string(cfg, buf) != CONFIG_TRUE) {
		ERROR("wrapper: failed to parse configuration (error: %s).",
		      config_error_text(cfg));
		return -1;
	}

	return 0;
}


static int wrapper_config_check(wrap_t *swu)
{
	config_t *cfg = &swu->cfg;
	config_setting_t *top;

	/*
	 * perform absolute minimum, almost content-unaware config check
	 */

	top = config_lookup(cfg, swu->type);

	if (top == NULL)
		goto missing_top_node;

	if (config_setting_index(top) != 0 ||
	    config_setting_get_elem(config_root_setting(cfg), 1) != NULL)
		goto unhandled_nodes;

	return 0;

 missing_top_node:
	ERROR("wrapper: malformed configuration, missing %s node.", swu->type);
	return -1;

 unhandled_nodes:
	ERROR("wrapper: malformed configuration, extra non-%s node.", swu->type);
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
	ERROR("wrapper: can't set env var, value too long");
	return NULL;

 invalid_type:
	ERROR("wrapper: can't set env var, invalid setting type (non-scalar).");
	return NULL;
}


static int setenv_scalar(wrap_env_t *e, const char *name, config_setting_t *cs)
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

	TRACE("wrapper: set env var '%s' = '%s'", e->name, e->value);

	return 0;

 nomem:
	ERROR("wrapper: can't set env var, out of memory");
	return -1;
}


static int setenv_group(wrap_env_t *envv, int envc, const char *prfx,
			config_setting_t *cs);
static int setenv_array(wrap_env_t *envv, int envc, const char *prfx,
			config_setting_t *cs);

static int setenv_group(wrap_env_t *envv, int envc, const char *prfx,
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
	ERROR("wrapper: can't set envvar (%s), invalid setting type.", name);
	return -1;
}


static int setenv_array(wrap_env_t *envv, int envc, const char *prfx,
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
	ERROR("wrapper: can't set envvar (%s), invalid setting type.", name);
	return -1;
}


static int wrapper_mkenv(wrap_t *swupd)
{
	int envc = sizeof(swupd->envv) / sizeof(swupd->envv[0]);

	envc = setenv_group(swupd->envv, envc, "",
			    config_root_setting(&swupd->cfg));

	if (envc < 0)
		return -1;

	swupd->envc = envc;

	return 0;
}


static int wrapper_setenv(wrap_t *swupd)
{
	struct img_type *img = swupd->img;
	wrap_env_t      *env = swupd->envv;
	const char      *var, *val;

	if (img->device[0]) {
		var = "SWUPDATE_IMAGE_DEVICE";
		val = img->device;
		TRACE("wrapper: set env var '%s' = '%s'", var, val);
		setenv(var, val, 1);
	}
	if (img->volname[0]) {
		var = "SWUPDATE_IMAGE_VOLNAME";
		val = img->volname;
		TRACE("wrapper: set env var '%s' = '%s'", var, val);
		setenv(var, val, 1);
	}

	for (; env->name != NULL; env++)
		setenv(env->name, env->value, 1);

	return 0;
}


static int wrapper_init(wrap_t *swu, struct img_type *img, void *data)
{
	if (data != swu) {
		ERROR("wrapper: swu != data, evacuating...");
		exit(1);
	}

	swu->type = img->type;
	swu->img  = img;

	wrapper_config_init(swu);

	if (wrapper_config_read(swu))
		return -1;

	if (wrapper_config_check(swu))
		return -1;

	return 0;
}


static void wrapper_exit(wrap_t *swu)
{
	swu->type = "<unknown>";
	wrapper_config_exit(swu);
}


static int install_wrapped_update(struct img_type *img, void *data)
{
	wrap_t *swu = data;
	int     status;

	INFO("wrapper: using hookdir '%s', main hook '%s'...",
	     swu->hookdir, swu->mainhook);

	wrapper_init(swu, img, data);
	status = wrapper_run_hooks(swu);
	wrapper_exit(swu);

	return status;
}


/*
 * Notes:
 *     Currently the wrapped update mechanisms are selected during
 *     configuration. If we want to, we can easily make the these
 *     dynamically detected upon startup by iterating through the
 *     subdirectories found in our hook directory and registering
 *     each one as a wrapped backend.
 */


static void wrapper_init_hooks(wrap_t *swu)
{
	char mainhook[PATH_MAX];
	const char *hookdir = getenv(HOOKDIR_OVERRIDE);

	if (hookdir == NULL) {
		swu->hookdir  = HOOKDIR;
		swu->mainhook = MAINHOOK;
	}
	else {
		snprintf(mainhook, sizeof(mainhook), "%s/run-hooks", hookdir);
		swu->hookdir  = strdup(hookdir);
		swu->mainhook = strdup(mainhook);
	}
}


static int register_backends(wrap_t *swu)
{
	DIR           *dir;
	struct dirent *de;
	char          *type;

	if ((dir = opendir(swu->hookdir)) == NULL)
		goto open_failed;

	errno = 0;
	while ((de = readdir(dir)) != NULL) {
		if (de == NULL)
			break;

		if (de->d_type != DT_DIR || de->d_name[0] == '.')
			continue;

		if ((type = strdup(de->d_name)) == NULL)
			goto nomem;

		if (register_handler(type, install_wrapped_update,
				     (void *)swu) < 0) {
			ERROR("failed to register handler for '%s'", type);
			free(type);
			goto register_failed;
		}
	}

	if (errno != 0)
		goto read_failed;

	closedir(dir);
	return 0;

 open_failed:
	ERROR("wrapper: failed to open '%s' for scanning backends (%d: %s)",
	      HOOKDIR, errno, strerror(errno));
	return -1;

 read_failed:
	ERROR("wrapper: failed to scan '%s' for backends (%d: %s)",
	      HOOKDIR, errno, strerror(errno));
	closedir(dir);
	return -1;

 register_failed:
	closedir(dir);
	return -1;

 nomem:
	closedir(dir);
	return -1;
}


__attribute__((constructor))
static void wrapper_handler(void)
{
	static wrap_t swu;

	memset(&swu, 0, sizeof(swu));
	wrapper_init_hooks(&swu);
	register_backends(&swu);
}
