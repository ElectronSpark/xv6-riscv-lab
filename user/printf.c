#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "errno.h"

#include <stdarg.h>

#define LOCAL_STACK_BUF 128

static int
write_all(int fd, const char *buf, size_t len)
{
	size_t written = 0;
	while (written < len) {
		ssize_t rc = write(fd, buf + written, len - written);
		if (rc < 0)
			return -1;
		if (rc == 0) {
			errno = EIO;
			return -1;
		}
		written += (size_t)rc;
	}
	return 0;
}

static int
vdprintf_fd(int fd, const char *fmt, va_list ap)
{
	char stack_buf[LOCAL_STACK_BUF];
	va_list ap_copy;
	va_copy(ap_copy, ap);
	int needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap_copy);
	va_end(ap_copy);
	if (needed < 0)
		return -1;

	if ((size_t)needed < sizeof(stack_buf)) {
		if (write_all(fd, stack_buf, (size_t)needed) < 0)
			return -1;
		return needed;
	}

	size_t buf_size = (size_t)needed + 1;
	char *heap_buf = (char *)malloc(buf_size);
	if (!heap_buf) {
		errno = ENOMEM;
		return -1;
	}

	va_list ap_copy2;
	va_copy(ap_copy2, ap);
	int formatted = vsnprintf(heap_buf, buf_size, fmt, ap_copy2);
	va_end(ap_copy2);
	if (formatted < 0) {
		free(heap_buf);
		return -1;
	}

	if (write_all(fd, heap_buf, (size_t)formatted) < 0) {
		free(heap_buf);
		return -1;
	}

	free(heap_buf);
	return formatted;
}

int
vprintf(int fd, const char *fmt, va_list ap)
{
	return vdprintf_fd(fd, fmt, ap);
}

int
fprintf(int fd, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int rc = vdprintf_fd(fd, fmt, ap);
	va_end(ap);
	return rc;
}

int
printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int rc = vdprintf_fd(1, fmt, ap);
	va_end(ap);
	return rc;
}
