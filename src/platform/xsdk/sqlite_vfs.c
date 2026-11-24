/* SQLite's operating-system layer on the Xbox 360, over xsdk's POSIX file calls (SQLITE_OS_OTHER).
 *
 * One process owns the database, so locking is a no-op (the title runs alone; SQLITE_DEFAULT_LOCKING_MODE=1 makes
 * SQLite take exclusive mode anyway). Journals are deleted or truncated as SQLite asks; the console can't remove a
 * file while it's open, which SQLite never does to its own files. Randomness comes from getentropy (the kernel's
 * RNG), time from gettimeofday, sleeps from nanosleep.
 *
 * Registered as the default VFS by sqlite3_os_init, which SQLITE_OS_OTHER leaves to us. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "sqlite3.h"

typedef struct {
	sqlite3_file base;
	int fd;
	int delete_on_close;
	char *path;
} XstFile;

static int xst_close(sqlite3_file *f)
{
	XstFile *x = (XstFile *)f;
	if (x->fd >= 0)
		close(x->fd);
	x->fd = -1;
	if (x->delete_on_close && x->path)
		unlink(x->path);
	sqlite3_free(x->path);
	x->path = NULL;
	return SQLITE_OK;
}

static int xst_read(sqlite3_file *f, void *buffer, int amount, sqlite3_int64 offset)
{
	XstFile *x = (XstFile *)f;
	int done = 0;
	while (done < amount) {
		const ssize_t n = pread(x->fd, (char *)buffer + done, (size_t)(amount - done), (off_t)(offset + done));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return SQLITE_IOERR_READ;
		}
		if (n == 0)
			break;
		done += (int)n;
	}
	if (done < amount) {
		memset((char *)buffer + done, 0, (size_t)(amount - done));
		return SQLITE_IOERR_SHORT_READ;
	}
	return SQLITE_OK;
}

static int xst_write(sqlite3_file *f, const void *buffer, int amount, sqlite3_int64 offset)
{
	XstFile *x = (XstFile *)f;
	int done = 0;
	while (done < amount) {
		const ssize_t n = pwrite(x->fd, (const char *)buffer + done, (size_t)(amount - done), (off_t)(offset + done));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return errno == ENOSPC ? SQLITE_FULL : SQLITE_IOERR_WRITE;
		}
		done += (int)n;
	}
	return SQLITE_OK;
}

static int xst_truncate(sqlite3_file *f, sqlite3_int64 size)
{
	return ftruncate(((XstFile *)f)->fd, (off_t)size) == 0 ? SQLITE_OK : SQLITE_IOERR_TRUNCATE;
}

static int xst_sync(sqlite3_file *f, int flags)
{
	(void)flags;
	return fsync(((XstFile *)f)->fd) == 0 ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}

static int xst_file_size(sqlite3_file *f, sqlite3_int64 *size)
{
	struct stat st;
	if (fstat(((XstFile *)f)->fd, &st) != 0)
		return SQLITE_IOERR_FSTAT;
	*size = (sqlite3_int64)st.st_size;
	return SQLITE_OK;
}

static int xst_lock(sqlite3_file *f, int level)
{
	(void)f;
	(void)level;
	return SQLITE_OK;
}

static int xst_check_reserved(sqlite3_file *f, int *out)
{
	(void)f;
	*out = 0;
	return SQLITE_OK;
}

static int xst_file_control(sqlite3_file *f, int op, void *arg)
{
	(void)f;
	(void)op;
	(void)arg;
	return SQLITE_NOTFOUND;
}

static int xst_sector_size(sqlite3_file *f)
{
	(void)f;
	return 4096; /* FATX and the drives' sectors are 512 bytes; 4 KB clusters */
}

static int xst_device_characteristics(sqlite3_file *f)
{
	(void)f;
	return 0;
}

static const sqlite3_io_methods kIoMethods = {
	1, xst_close, xst_read, xst_write, xst_truncate, xst_sync, xst_file_size, xst_lock, xst_lock,
	xst_check_reserved, xst_file_control, xst_sector_size, xst_device_characteristics,
	NULL, NULL, NULL, NULL, NULL, NULL,
};

static int xst_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *f, int flags, int *out_flags)
{
	(void)vfs;
	XstFile *x = (XstFile *)f;
	memset(x, 0, sizeof *x);
	x->fd = -1;
	char temp[64];
	if (!name) {
		/* A temporary file SQLite asks for without a name: beside the title, removed on close. */
		static unsigned counter;
		snprintf(temp, sizeof temp, "game:/xst-sqlite-%u.tmp", ++counter);
		name = temp;
		flags |= SQLITE_OPEN_DELETEONCLOSE;
	}
	int oflags = 0;
	if (flags & SQLITE_OPEN_READWRITE)
		oflags |= O_RDWR;
	else
		oflags |= O_RDONLY;
	if (flags & SQLITE_OPEN_CREATE)
		oflags |= O_CREAT;
	if (flags & SQLITE_OPEN_EXCLUSIVE)
		oflags |= O_EXCL;
	const int fd = open(name, oflags, 0644);
	if (fd < 0)
		return SQLITE_CANTOPEN;
	x->fd = fd;
	x->delete_on_close = (flags & SQLITE_OPEN_DELETEONCLOSE) != 0;
	if (x->delete_on_close)
		x->path = sqlite3_mprintf("%s", name);
	if (out_flags)
		*out_flags = flags;
	x->base.pMethods = &kIoMethods;
	return SQLITE_OK;
}

static int xst_delete(sqlite3_vfs *vfs, const char *name, int sync_dir)
{
	(void)vfs;
	(void)sync_dir;
	if (unlink(name) == 0 || errno == ENOENT)
		return SQLITE_OK;
	return SQLITE_IOERR_DELETE;
}

static int xst_access(sqlite3_vfs *vfs, const char *name, int flags, int *out)
{
	(void)vfs;
	(void)flags;
	struct stat st;
	*out = stat(name, &st) == 0 && (flags != SQLITE_ACCESS_EXISTS || st.st_size > 0 || S_ISREG(st.st_mode));
	return SQLITE_OK;
}

static int xst_full_pathname(sqlite3_vfs *vfs, const char *name, int out_size, char *out)
{
	(void)vfs;
	/* xsdk's paths are drive-rooted (hdd:/...); take them as they are. */
	sqlite3_snprintf(out_size, out, "%s", name);
	return SQLITE_OK;
}

static int xst_randomness(sqlite3_vfs *vfs, int size, char *out)
{
	(void)vfs;
	int done = 0;
	while (done < size) {
		const int chunk = size - done > 256 ? 256 : size - done;
		if (getentropy(out + done, (size_t)chunk) != 0)
			break;
		done += chunk;
	}
	return done;
}

static int xst_sleep(sqlite3_vfs *vfs, int microseconds)
{
	(void)vfs;
	struct timespec ts = { microseconds / 1000000, (long)(microseconds % 1000000) * 1000 };
	nanosleep(&ts, NULL);
	return microseconds;
}

static int xst_current_time_int64(sqlite3_vfs *vfs, sqlite3_int64 *out)
{
	(void)vfs;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	/* Julian day number in milliseconds. */
	*out = (sqlite3_int64)210866760000000LL + (sqlite3_int64)tv.tv_sec * 1000 + tv.tv_usec / 1000;
	return SQLITE_OK;
}

static int xst_current_time(sqlite3_vfs *vfs, double *out)
{
	sqlite3_int64 ms;
	xst_current_time_int64(vfs, &ms);
	*out = (double)ms / 86400000.0;
	return SQLITE_OK;
}

static int xst_get_last_error(sqlite3_vfs *vfs, int size, char *out)
{
	(void)vfs;
	if (size > 0)
		sqlite3_snprintf(size, out, "%s", strerror(errno));
	return errno;
}

static sqlite3_vfs kVfs = {
	2, sizeof(XstFile), 520, NULL, "xsdk", NULL, xst_open, xst_delete, xst_access, xst_full_pathname,
	NULL, NULL, NULL, NULL, xst_randomness, xst_sleep, xst_current_time, xst_get_last_error,
	xst_current_time_int64, NULL, NULL, NULL,
};

int sqlite3_os_init(void) { return sqlite3_vfs_register(&kVfs, 1); }

int sqlite3_os_end(void) { return SQLITE_OK; }
