/*
 * lxc: linux Container library
 *
 * (C) Copyright IBM Corp. 2007, 2008
 *
 * Authors:
 * Daniel Lezcano <daniel.lezcano at free.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <assert.h>

#ifndef HAVE_GETLINE
#ifdef HAVE_FGETLN
#include <../include/getline.h>
#endif
#endif

#include "utils.h"
#include "log.h"
#include "lxclock.h"

#define MAX_STACKDEPTH 25

lxc_log_define(lxc_utils, lxc);

static int _recursive_rmdir_onedev(char *dirname, dev_t pdev)
{
	struct dirent dirent, *direntp;
	DIR *dir;
	int ret, failed=0;
	char pathname[MAXPATHLEN];

	dir = opendir(dirname);
	if (!dir) {
		ERROR("%s: failed to open %s", __func__, dirname);
		return -1;
	}

	while (!readdir_r(dir, &dirent, &direntp)) {
		struct stat mystat;
		int rc;

		if (!direntp)
			break;

		if (!strcmp(direntp->d_name, ".") ||
		    !strcmp(direntp->d_name, ".."))
			continue;

		rc = snprintf(pathname, MAXPATHLEN, "%s/%s", dirname, direntp->d_name);
		if (rc < 0 || rc >= MAXPATHLEN) {
			ERROR("pathname too long");
			failed=1;
			continue;
		}
		ret = lstat(pathname, &mystat);
		if (ret) {
			ERROR("%s: failed to stat %s", __func__, pathname);
			failed=1;
			continue;
		}
		if (mystat.st_dev != pdev)
			continue;
		if (S_ISDIR(mystat.st_mode)) {
			if (_recursive_rmdir_onedev(pathname, pdev) < 0)
				failed=1;
		} else {
			if (unlink(pathname) < 0) {
				ERROR("%s: failed to delete %s", __func__, pathname);
				failed=1;
			}
		}
	}

	if (rmdir(dirname) < 0) {
		ERROR("%s: failed to delete %s", __func__, dirname);
		failed=1;
	}

	ret = closedir(dir);
	if (ret) {
		ERROR("%s: failed to close directory %s", __func__, dirname);
		failed=1;
	}

	return failed ? -1 : 0;
}

/* returns 0 on success, -1 if there were any failures */
extern int lxc_rmdir_onedev(char *path)
{
	struct stat mystat;

	if (lstat(path, &mystat) < 0) {
		ERROR("%s: failed to stat %s", __func__, path);
		return -1;
	}

	return _recursive_rmdir_onedev(path, mystat.st_dev);
}

static int mount_fs(const char *source, const char *target, const char *type)
{
	/* the umount may fail */
	if (umount(target))
		WARN("failed to unmount %s : %s", target, strerror(errno));

	if (mount(source, target, type, 0, NULL)) {
		ERROR("failed to mount %s : %s", target, strerror(errno));
		return -1;
	}

	DEBUG("'%s' mounted on '%s'", source, target);

	return 0;
}

extern void lxc_setup_fs(void)
{
	if (mount_fs("proc", "/proc", "proc"))
		INFO("failed to remount proc");

	/* if we can't mount /dev/shm, continue anyway */
	if (mount_fs("shmfs", "/dev/shm", "tmpfs"))
		INFO("failed to mount /dev/shm");

	/* If we were able to mount /dev/shm, then /dev exists */
	/* Sure, but it's read-only per config :) */
	if (access("/dev/mqueue", F_OK) && mkdir("/dev/mqueue", 0666)) {
		DEBUG("failed to create '/dev/mqueue'");
		return;
	}

	/* continue even without posix message queue support */
	if (mount_fs("mqueue", "/dev/mqueue", "mqueue"))
		INFO("failed to mount /dev/mqueue");
}

/* borrowed from iproute2 */
extern int get_u16(unsigned short *val, const char *arg, int base)
{
	unsigned long res;
	char *ptr;

	if (!arg || !*arg)
		return -1;

	errno = 0;
	res = strtoul(arg, &ptr, base);
	if (!ptr || ptr == arg || *ptr || res > 0xFFFF || errno != 0)
		return -1;

	*val = res;

	return 0;
}

extern int mkdir_p(const char *dir, mode_t mode)
{
	const char *tmp = dir;
	const char *orig = dir;
	char *makeme;

	do {
		dir = tmp + strspn(tmp, "/");
		tmp = dir + strcspn(dir, "/");
		makeme = strndup(orig, dir - orig);
		if (*makeme) {
			if (mkdir(makeme, mode) && errno != EEXIST) {
				SYSERROR("failed to create directory '%s'\n", makeme);
				free(makeme);
				return -1;
			}
		}
		free(makeme);
	} while(tmp != dir);

	return 0;
}

extern void remove_trailing_slashes(char *p)
{
	int l = strlen(p);
	while (--l >= 0 && (p[l] == '/' || p[l] == '\n'))
		p[l] = '\0';
}

static char *copy_global_config_value(char *p)
{
	int len = strlen(p);
	char *retbuf;

	if (len < 1)
		return NULL;
	if (p[len-1] == '\n') {
		p[len-1] = '\0';
		len--;
	}
	retbuf = malloc(len+1);
	if (!retbuf)
		return NULL;
	strcpy(retbuf, p);
	return retbuf;
}

#define DEFAULT_VG "lxc"
#define DEFAULT_THIN_POOL "lxc"
#define DEFAULT_ZFSROOT "lxc"

const char *lxc_global_config_value(const char *option_name)
{
	static const char *options[][2] = {
		{ "lvm_vg",          DEFAULT_VG      },
		{ "lvm_thin_pool",   DEFAULT_THIN_POOL },
		{ "zfsroot",         DEFAULT_ZFSROOT },
		{ "lxcpath",         NULL            },
		{ "cgroup.pattern",  DEFAULT_CGROUP_PATTERN },
		{ "cgroup.use",      NULL            },
		{ NULL, NULL },
	};

	/* Protected by a mutex to eliminate conflicting load and store operations */
	static const char *values[sizeof(options) / sizeof(options[0])] = { 0 };

	char *user_config_path = NULL;
	char *user_lxc_path = NULL;

	if (geteuid() > 0) {
		const char *user_home = getenv("HOME");
		if (!user_home)
			user_home = "/";

		user_config_path = malloc(sizeof(char) * (22 + strlen(user_home)));
		user_lxc_path = malloc(sizeof(char) * (19 + strlen(user_home)));

		sprintf(user_config_path, "%s/.config/lxc/lxc.conf", user_home);
		sprintf(user_lxc_path, "%s/.local/share/lxc/", user_home);
	}
	else {
		user_config_path = strdup(LXC_GLOBAL_CONF);
		user_lxc_path = strdup(LXCPATH);
	}

	const char *(*ptr)[2];
	const char *value;
	size_t i;
	char buf[1024], *p, *p2;
	FILE *fin = NULL;

	for (i = 0, ptr = options; (*ptr)[0]; ptr++, i++) {
		if (!strcmp(option_name, (*ptr)[0]))
			break;
	}
	if (!(*ptr)[0]) {
		free(user_config_path);
		free(user_lxc_path);
		errno = EINVAL;
		return NULL;
	}

	static_lock();
	if (values[i]) {
		free(user_config_path);
		free(user_lxc_path);
		value = values[i];
		static_unlock();
		return value;
	}
	static_unlock();

	fin = fopen_cloexec(user_config_path, "r");
	free(user_config_path);
	if (fin) {
		while (fgets(buf, 1024, fin)) {
			if (buf[0] == '#')
				continue;
			p = strstr(buf, option_name);
			if (!p)
				continue;
			/* see if there was just white space in front
			 * of the option name
			 */
			for (p2 = buf; p2 < p; p2++) {
				if (*p2 != ' ' && *p2 != '\t')
					break;
			}
			if (p2 < p)
				continue;
			p = strchr(p, '=');
			if (!p)
				continue;
			/* see if there was just white space after
			 * the option name
			 */
			for (p2 += strlen(option_name); p2 < p; p2++) {
				if (*p2 != ' ' && *p2 != '\t')
					break;
			}
			if (p2 < p)
				continue;
			p++;
			while (*p && (*p == ' ' || *p == '\t')) p++;
			if (!*p)
				continue;
			static_lock();
			values[i] = copy_global_config_value(p);
			static_unlock();
			free(user_lxc_path);
			goto out;
		}
	}
	/* could not find value, use default */
	static_lock();
	if (strcmp(option_name, "lxcpath") == 0)
		values[i] = user_lxc_path;
	else {
		free(user_lxc_path);
		values[i] = (*ptr)[1];
	}
	/* special case: if default value is NULL,
	 * and there is no config, don't view that
	 * as an error... */
	if (!values[i])
		errno = 0;
	static_unlock();

out:
	if (fin)
		fclose(fin);

	static_lock();
	value = values[i];
	static_unlock();
	return value;
}

const char *default_lvm_vg(void)
{
	return lxc_global_config_value("lvm_vg");
}

const char *default_lvm_thin_pool(void)
{
	return lxc_global_config_value("lvm_thin_pool");
}

const char *default_zfs_root(void)
{
	return lxc_global_config_value("zfsroot");
}

const char *default_lxc_path(void)
{
	return lxc_global_config_value("lxcpath");
}

const char *default_cgroup_use(void)
{
	return lxc_global_config_value("cgroup.use");
}

const char *default_cgroup_pattern(void)
{
	return lxc_global_config_value("cgroup.pattern");
}

const char *get_rundir()
{
	const char *rundir;

	rundir = getenv("XDG_RUNTIME_DIR");
	if (geteuid() == 0 || rundir == NULL)
		rundir = "/run";
	return rundir;
}

int wait_for_pid(pid_t pid)
{
	int status, ret;

again:
	ret = waitpid(pid, &status, 0);
	if (ret == -1) {
		if (errno == EINTR)
			goto again;
		return -1;
	}
	if (ret != pid)
		goto again;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

int lxc_wait_for_pid_status(pid_t pid)
{
	int status, ret;

again:
	ret = waitpid(pid, &status, 0);
	if (ret == -1) {
		if (errno == EINTR)
			goto again;
		return -1;
	}
	if (ret != pid)
		goto again;
	return status;
}

ssize_t lxc_write_nointr(int fd, const void* buf, size_t count)
{
	ssize_t ret;
again:
	ret = write(fd, buf, count);
	if (ret < 0 && errno == EINTR)
		goto again;
	return ret;
}

ssize_t lxc_read_nointr(int fd, void* buf, size_t count)
{
	ssize_t ret;
again:
	ret = read(fd, buf, count);
	if (ret < 0 && errno == EINTR)
		goto again;
	return ret;
}

ssize_t lxc_read_nointr_expect(int fd, void* buf, size_t count, const void* expected_buf)
{
	ssize_t ret;
	ret = lxc_read_nointr(fd, buf, count);
	if (ret <= 0)
		return ret;
	if ((size_t)ret != count)
		return -1;
	if (expected_buf && memcmp(buf, expected_buf, count) != 0) {
		errno = EINVAL;
		return -1;
	}
	return ret;
}

#if HAVE_LIBGNUTLS
#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>

__attribute__((constructor))
static void gnutls_lxc_init(void)
{
	gnutls_global_init();
}

int sha1sum_file(char *fnam, unsigned char *digest)
{
	char *buf;
	int ret;
	FILE *f;
	long flen;

	if (!fnam)
		return -1;
	f = fopen_cloexec(fnam, "r");
	if (!f) {
		SYSERROR("Error opening template");
		return -1;
	}
	if (fseek(f, 0, SEEK_END) < 0) {
		SYSERROR("Error seeking to end of template");
		fclose(f);
		return -1;
	}
	if ((flen = ftell(f)) < 0) {
		SYSERROR("Error telling size of template");
		fclose(f);
		return -1;
	}
	if (fseek(f, 0, SEEK_SET) < 0) {
		SYSERROR("Error seeking to start of template");
		fclose(f);
		return -1;
	}
	if ((buf = malloc(flen+1)) == NULL) {
		SYSERROR("Out of memory");
		fclose(f);
		return -1;
	}
	if (fread(buf, 1, flen, f) != flen) {
		SYSERROR("Failure reading template");
		free(buf);
		fclose(f);
		return -1;
	}
	if (fclose(f) < 0) {
		SYSERROR("Failre closing template");
		free(buf);
		return -1;
	}
	buf[flen] = '\0';
	ret = gnutls_hash_fast(GNUTLS_DIG_SHA1, buf, flen, (void *)digest);
	free(buf);
	return ret;
}
#endif

char** lxc_va_arg_list_to_argv(va_list ap, size_t skip, int do_strdup)
{
	va_list ap2;
	size_t count = 1 + skip;
	char **result;

	/* first determine size of argument list, we don't want to reallocate
	 * constantly...
	 */
	va_copy(ap2, ap);
	while (1) {
		char* arg = va_arg(ap2, char*);
		if (!arg)
			break;
		count++;
	}
	va_end(ap2);

	result = calloc(count, sizeof(char*));
	if (!result)
		return NULL;
	count = skip;
	while (1) {
		char* arg = va_arg(ap, char*);
		if (!arg)
			break;
		arg = do_strdup ? strdup(arg) : arg;
		if (!arg)
			goto oom;
		result[count++] = arg;
	}

	/* calloc has already set last element to NULL*/
	return result;

oom:
	free(result);
	return NULL;
}

const char** lxc_va_arg_list_to_argv_const(va_list ap, size_t skip)
{
	return (const char**)lxc_va_arg_list_to_argv(ap, skip, 0);
}

FILE *fopen_cloexec(const char *path, const char *mode)
{
	int open_mode = 0;
	int step = 0;
	int fd;
	int saved_errno = 0;
	FILE *ret;

	if (!strncmp(mode, "r+", 2)) {
		open_mode = O_RDWR;
		step = 2;
	} else if (!strncmp(mode, "r", 1)) {
		open_mode = O_RDONLY;
		step = 1;
	} else if (!strncmp(mode, "w+", 2)) {
		open_mode = O_RDWR | O_TRUNC | O_CREAT;
		step = 2;
	} else if (!strncmp(mode, "w", 1)) {
		open_mode = O_WRONLY | O_TRUNC | O_CREAT;
		step = 1;
	} else if (!strncmp(mode, "a+", 2)) {
		open_mode = O_RDWR | O_CREAT | O_APPEND;
		step = 2;
	} else if (!strncmp(mode, "a", 1)) {
		open_mode = O_WRONLY | O_CREAT | O_APPEND;
		step = 1;
	}
	for (; mode[step]; step++)
		if (mode[step] == 'x')
			open_mode |= O_EXCL;
	open_mode |= O_CLOEXEC;

	fd = open(path, open_mode, 0666);
	if (fd < 0)
		return NULL;

	ret = fdopen(fd, mode);
	saved_errno = errno;
	if (!ret)
		close(fd);
	errno = saved_errno;
	return ret;
}

extern struct lxc_popen_FILE *lxc_popen(const char *command)
{
	struct lxc_popen_FILE *fp = NULL;
	int parent_end = -1, child_end = -1;
	int pipe_fds[2];
	pid_t child_pid;

	int r = pipe2(pipe_fds, O_CLOEXEC);

	if (r < 0) {
		ERROR("pipe2 failure");
		return NULL;
	}

	parent_end = pipe_fds[0];
	child_end = pipe_fds[1];

	child_pid = fork();

	if (child_pid == 0) {
		/* child */
		int child_std_end = STDOUT_FILENO;

		if (child_end != child_std_end) {
			/* dup2() doesn't dup close-on-exec flag */
			dup2(child_end, child_std_end);

			/* it's safe not to close child_end here
			 * as it's marked close-on-exec anyway
			 */
		} else {
			/*
			 * The descriptor is already the one we will use.
			 * But it must not be marked close-on-exec.
			 * Undo the effects.
			 */
			fcntl(child_end, F_SETFD, 0);
		}

		/*
		 * Unblock signals.
		 * This is the main/only reason
		 * why we do our lousy popen() emulation.
		 */
		{
			sigset_t mask;
			sigfillset(&mask);
			sigprocmask(SIG_UNBLOCK, &mask, NULL);
		}

		execl("/bin/sh", "sh", "-c", command, (char *) NULL);
		exit(127);
	}

	/* parent */

	close(child_end);
	child_end = -1;

	if (child_pid < 0) {
		ERROR("fork failure");
		goto error;
	}

	fp = calloc(1, sizeof(*fp));
	if (!fp) {
		ERROR("failed to allocate memory");
		goto error;
	}

	fp->f = fdopen(parent_end, "r");
	if (!fp->f) {
		ERROR("fdopen failure");
		goto error;
	}

	fp->child_pid = child_pid;

	return fp;

error:

	if (fp) {
		if (fp->f) {
			fclose(fp->f);
			parent_end = -1; /* so we do not close it second time */
		}

		free(fp);
	}

	if (parent_end != -1)
		close(parent_end);

	return NULL;
}

extern int lxc_pclose(struct lxc_popen_FILE *fp)
{
	FILE *f = NULL;
	pid_t child_pid = 0;
	int wstatus = 0;
	pid_t wait_pid;

	if (fp) {
		f = fp->f;
		child_pid = fp->child_pid;
		/* free memory (we still need to close file stream) */
		free(fp);
		fp = NULL;
	}

	if (!f || fclose(f)) {
		ERROR("fclose failure");
		return -1;
	}

	do {
		wait_pid = waitpid(child_pid, &wstatus, 0);
	} while (wait_pid == -1 && errno == EINTR);

	if (wait_pid == -1) {
		ERROR("waitpid failure");
		return -1;
	}

	return wstatus;
}

char *lxc_string_replace(const char *needle, const char *replacement, const char *haystack)
{
	ssize_t len = -1, saved_len = -1;
	char *result = NULL;
	size_t replacement_len = strlen(replacement);
	size_t needle_len = strlen(needle);

	/* should be executed exactly twice */
	while (len == -1 || result == NULL) {
		char *p;
		char *last_p;
		ssize_t part_len;

		if (len != -1) {
			result = calloc(1, len + 1);
			if (!result)
				return NULL;
			saved_len = len;
		}

		len = 0;

		for (last_p = (char *)haystack, p = strstr(last_p, needle); p; last_p = p, p = strstr(last_p, needle)) {
			part_len = (ssize_t)(p - last_p);
			if (result && part_len > 0)
				memcpy(&result[len], last_p, part_len);
			len += part_len;
			if (result && replacement_len > 0)
				memcpy(&result[len], replacement, replacement_len);
			len += replacement_len;
			p += needle_len;
		}
		part_len = strlen(last_p);
		if (result && part_len > 0)
			memcpy(&result[len], last_p, part_len);
		len += part_len;
	}

	/* make sure we did the same thing twice,
	 * once for calculating length, the other
	 * time for copying data */
	assert(saved_len == len);
	/* make sure we didn't overwrite any buffer,
	 * due to calloc the string should be 0-terminated */
	assert(result[len] == '\0');

	return result;
}

bool lxc_string_in_array(const char *needle, const char **haystack)
{
	for (; haystack && *haystack; haystack++)
		if (!strcmp(needle, *haystack))
			return true;
	return false;
}

char *lxc_string_join(const char *sep, const char **parts, bool use_as_prefix)
{
	char *result;
	char **p;
	size_t sep_len = strlen(sep);
	size_t result_len = use_as_prefix * sep_len;

	/* calculate new string length */
	for (p = (char **)parts; *p; p++)
		result_len += (p > (char **)parts) * sep_len + strlen(*p);

	result = calloc(result_len + 1, 1);
	if (!result)
		return NULL;

	if (use_as_prefix)
		strcpy(result, sep);
	for (p = (char **)parts; *p; p++) {
		if (p > (char **)parts)
			strcat(result, sep);
		strcat(result, *p);
	}

	return result;
}

char **lxc_normalize_path(const char *path)
{
	char **components;
	char **p;
	size_t components_len = 0;
	size_t pos = 0;

	components = lxc_string_split(path, '/');
	if (!components)
		return NULL;
	for (p = components; *p; p++)
		components_len++;

	/* resolve '.' and '..' */
	for (pos = 0; pos < components_len; ) {
		if (!strcmp(components[pos], ".") || (!strcmp(components[pos], "..") && pos == 0)) {
			/* eat this element */
			free(components[pos]);
			memmove(&components[pos], &components[pos+1], sizeof(char *) * (components_len - pos));
			components_len--;
		} else if (!strcmp(components[pos], "..")) {
			/* eat this and the previous element */
			free(components[pos - 1]);
			free(components[pos]);
			memmove(&components[pos-1], &components[pos+1], sizeof(char *) * (components_len - pos));
			components_len -= 2;
			pos--;
		} else {
			pos++;
		}
	}

	return components;
}

char *lxc_append_paths(const char *first, const char *second)
{
	size_t len = strlen(first) + strlen(second) + 1;
	const char *pattern = "%s%s";
	char *result = NULL;

	if (second[0] != '/') {
		len += 1;
		pattern = "%s/%s";
	}

	result = calloc(1, len);
	if (!result)
		return NULL;

	snprintf(result, len, pattern, first, second);
	return result;
}

bool lxc_string_in_list(const char *needle, const char *haystack, char _sep)
{
	char *token, *str, *saveptr = NULL;
	char sep[2] = { _sep, '\0' };

	if (!haystack || !needle)
		return 0;

	str = alloca(strlen(haystack)+1);
	strcpy(str, haystack);
	for (; (token = strtok_r(str, sep, &saveptr)); str = NULL) {
		if (strcmp(needle, token) == 0)
			return 1;
	}

	return 0;
}

char **lxc_string_split(const char *string, char _sep)
{
	char *token, *str, *saveptr = NULL;
	char sep[2] = { _sep, '\0' };
	char **result = NULL;
	size_t result_capacity = 0;
	size_t result_count = 0;
	int r, saved_errno;

	if (!string)
		return calloc(1, sizeof(char *));

	str = alloca(strlen(string)+1);
	strcpy(str, string);
	for (; (token = strtok_r(str, sep, &saveptr)); str = NULL) {
		r = lxc_grow_array((void ***)&result, &result_capacity, result_count + 1, 16);
		if (r < 0)
			goto error_out;
		result[result_count] = strdup(token);
		if (!result[result_count])
			goto error_out;
		result_count++;
	}

	/* if we allocated too much, reduce it */
	return realloc(result, (result_count + 1) * sizeof(char *));
error_out:
	saved_errno = errno;
	lxc_free_array((void **)result, free);
	errno = saved_errno;
	return NULL;
}

char **lxc_string_split_and_trim(const char *string, char _sep)
{
	char *token, *str, *saveptr = NULL;
	char sep[2] = { _sep, '\0' };
	char **result = NULL;
	size_t result_capacity = 0;
	size_t result_count = 0;
	int r, saved_errno;
	size_t i = 0;

	if (!string)
		return calloc(1, sizeof(char *));

	str = alloca(strlen(string)+1);
	strcpy(str, string);
	for (; (token = strtok_r(str, sep, &saveptr)); str = NULL) {
		while (token[0] == ' ' || token[0] == '\t')
			token++;
		i = strlen(token);
		while (i > 0 && (token[i - 1] == ' ' || token[i - 1] == '\t')) {
			token[i - 1] = '\0';
			i--;
		}
		r = lxc_grow_array((void ***)&result, &result_capacity, result_count + 1, 16);
		if (r < 0)
			goto error_out;
		result[result_count] = strdup(token);
		if (!result[result_count])
			goto error_out;
		result_count++;
	}

	/* if we allocated too much, reduce it */
	return realloc(result, (result_count + 1) * sizeof(char *));
error_out:
	saved_errno = errno;
	lxc_free_array((void **)result, free);
	errno = saved_errno;
	return NULL;
}

void lxc_free_array(void **array, lxc_free_fn element_free_fn)
{
	void **p;
	for (p = array; p && *p; p++)
		element_free_fn(*p);
	free((void*)array);
}

int lxc_grow_array(void ***array, size_t* capacity, size_t new_size, size_t capacity_increment)
{
	size_t new_capacity;
	void **new_array;

	/* first time around, catch some trivial mistakes of the user
	 * only initializing one of these */
	if (!*array || !*capacity) {
		*array = NULL;
		*capacity = 0;
	}

	new_capacity = *capacity;
	while (new_size + 1 > new_capacity)
		new_capacity += capacity_increment;
	if (new_capacity != *capacity) {
		/* we have to reallocate */
		new_array = realloc(*array, new_capacity * sizeof(void *));
		if (!new_array)
			return -1;
		memset(&new_array[*capacity], 0, (new_capacity - (*capacity)) * sizeof(void *));
		*array = new_array;
		*capacity = new_capacity;
	}

	/* array has sufficient elements */
	return 0;
}

size_t lxc_array_len(void **array)
{
	void **p;
	size_t result = 0;

	for (p = array; p && *p; p++)
		result++;

	return result;
}

int lxc_write_to_file(const char *filename, const void* buf, size_t count, bool add_newline)
{
	int fd, saved_errno;
	ssize_t ret;

	fd = open(filename, O_WRONLY | O_TRUNC | O_CREAT | O_CLOEXEC, 0666);
	if (fd < 0)
		return -1;
	ret = lxc_write_nointr(fd, buf, count);
	if (ret < 0)
		goto out_error;
	if ((size_t)ret != count)
		goto out_error;
	if (add_newline) {
		ret = lxc_write_nointr(fd, "\n", 1);
		if (ret != 1)
			goto out_error;
	}
	close(fd);
	return 0;

out_error:
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return -1;
}

int lxc_read_from_file(const char *filename, void* buf, size_t count)
{
	int fd = -1, saved_errno;
	ssize_t ret;

	fd = open(filename, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	if (!buf || !count) {
		char buf2[100];
		size_t count2 = 0;
		while ((ret = read(fd, buf2, 100)) > 0)
			count2 += ret;
		if (ret >= 0)
			ret = count2;
	} else {
		memset(buf, 0, count);
		ret = read(fd, buf, count);
	}

	if (ret < 0)
		ERROR("read %s: %s", filename, strerror(errno));

	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return ret;
}

void **lxc_append_null_to_array(void **array, size_t count)
{
	void **temp;

	/* Append NULL to the array */
	if (count) {
		temp = realloc(array, (count + 1) * sizeof(*array));
		if (!temp) {
			int i;
			for (i = 0; i < count; i++)
				free(array[i]);
			free(array);
			return NULL;
		}
		array = temp;
		array[count] = NULL;
	}
	return array;
}
