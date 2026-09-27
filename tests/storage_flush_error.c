// SPDX-License-Identifier: GPL-2.0
/* Test-only preload library: fail one explicitly selected regular-image flush. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <unistd.h>

static atomic_int selected_fd = ATOMIC_VAR_INIT(-1);
static atomic_uint failures;

void bch_test_fail_next_flush(int fd)
{
	atomic_store(&failures, 0);
	atomic_store(&selected_fd, fd);
}

unsigned bch_test_flush_failures(void)
{
	return atomic_load(&failures);
}

int fdatasync(int fd)
{
	int expected = fd;
	if (fd >= 0 && atomic_compare_exchange_strong(&selected_fd, &expected, -1)) {
		atomic_fetch_add(&failures, 1);
		errno = EIO;
		return -1;
	}
	return syscall(SYS_fdatasync, fd);
}
