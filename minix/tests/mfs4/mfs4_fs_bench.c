#define _POSIX_C_SOURCE 200809L

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define SMALL_WRITE_SIZE 4096U
#define SEQUENTIAL_CHUNK (1024U * 1024U)

static int
make_path(char *dst, size_t size, const char *dir, const char *name)
{
	int n = snprintf(dst, size, "%s/%s", dir, name);

	if (n < 0 || (size_t)n >= size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int
write_all(int fd, const void *buffer, size_t length)
{
	const uint8_t *p = buffer;
	ssize_t n;

	while (length != 0) {
		n = write(fd, p, length);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			if (n == 0)
				errno = EIO;
			return -1;
		}
		p += n;
		length -= (size_t)n;
	}
	return 0;
}

static int
now_ns(uint64_t *value)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return -1;
	*value = (uint64_t)ts.tv_sec * 1000000000ULL +
	    (uint64_t)ts.tv_nsec;
	return 0;
}

static void
report_result(const char *label, const char *workload, uint64_t operations,
    uint64_t bytes, uint64_t elapsed_ns)
{
	double seconds = (double)elapsed_ns / 1000000000.0;
	double ops_per_sec = seconds > 0.0 ? (double)operations / seconds : 0.0;
	double mib_per_sec = seconds > 0.0 ?
	    ((double)bytes / (1024.0 * 1024.0)) / seconds : 0.0;
	uint64_t elapsed_us = elapsed_ns / 1000U;

	printf("%s,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64
	    ",%.2f,%.2f\n", label, workload, operations, bytes,
	    elapsed_us, ops_per_sec, mib_per_sec);
}

static int
bench_create_unlink(const char *dir, const char *label, uint64_t count)
{
	char path[PATH_MAX];
	uint64_t start, end, i;
	int fd;

	if (now_ns(&start) != 0)
		return -1;
	for (i = 0; i < count; i++) {
		char name[64];
		(void)snprintf(name, sizeof(name), "create-%" PRIu64, i);
		if (make_path(path, sizeof(path), dir, name) != 0)
			return -1;
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd < 0)
			return -1;
		if (write_all(fd, "x", 1) != 0) {
			int saved = errno;
			(void)close(fd);
			errno = saved;
			return -1;
		}
		if (close(fd) != 0 || unlink(path) != 0)
			return -1;
	}
	if (now_ns(&end) != 0)
		return -1;
	report_result(label, "create_unlink", count * 2, count,
	    end - start);
	return 0;
}

static int
bench_rename(const char *dir, const char *label, uint64_t count)
{
	char from[PATH_MAX], to[PATH_MAX];
	uint64_t start, end, i;
	int fd;

	if (make_path(from, sizeof(from), dir, "rename-a") != 0 ||
	    make_path(to, sizeof(to), dir, "rename-b") != 0)
		return -1;
	fd = open(from, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		return -1;
	if (close(fd) != 0)
		return -1;
	if (now_ns(&start) != 0)
		return -1;
	for (i = 0; i < count; i++) {
		if (rename(from, to) != 0 || rename(to, from) != 0)
			return -1;
	}
	if (now_ns(&end) != 0)
		return -1;
	if (unlink(from) != 0)
		return -1;
	report_result(label, "rename", count * 2, count * 2,
	    end - start);
	return 0;
}

static int
bench_small_write(const char *dir, const char *label, uint64_t count)
{
	char path[PATH_MAX];
	uint8_t buffer[SMALL_WRITE_SIZE];
	uint64_t start, end, i, writes = count * 16;
	int fd, r = -1;

	memset(buffer, 0x5a, sizeof(buffer));
	if (make_path(path, sizeof(path), dir, "small-write") != 0)
		return -1;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	if (now_ns(&start) != 0)
		goto out;
	for (i = 0; i < writes; i++) {
		if (write_all(fd, buffer, sizeof(buffer)) != 0)
			goto out;
	}
	if (now_ns(&end) != 0)
		goto out;
	report_result(label, "small_write_4k", writes,
	    writes * sizeof(buffer), end - start);
	r = 0;
out:
	{
		int saved = errno;
		if (close(fd) != 0 && r == 0)
			return -1;
		errno = saved;
	}
	return r;
}

static int
bench_sequential_write(const char *dir, const char *label,
    uint64_t mebibytes)
{
	char path[PATH_MAX];
	uint8_t buffer[SEQUENTIAL_CHUNK];
	uint64_t start, end, i;
	int fd, r = -1;

	memset(buffer, 0xa5, sizeof(buffer));
	if (make_path(path, sizeof(path), dir, "sequential-write") != 0)
		return -1;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	if (now_ns(&start) != 0)
		goto out;
	for (i = 0; i < mebibytes; i++) {
		if (write_all(fd, buffer, sizeof(buffer)) != 0)
			goto out;
	}
	if (fsync(fd) != 0 || now_ns(&end) != 0)
		goto out;
	report_result(label, "sequential_write_1m", mebibytes,
	    mebibytes * SEQUENTIAL_CHUNK, end - start);
	r = 0;
out:
	{
		int saved = errno;
		if (close(fd) != 0 && r == 0)
			return -1;
		errno = saved;
	}
	return r;
}

static int
bench_fsync(const char *dir, const char *label, uint64_t count)
{
	char path[PATH_MAX];
	uint8_t buffer[SMALL_WRITE_SIZE];
	uint64_t start, end, i;
	int fd, r = -1;

	memset(buffer, 0x3c, sizeof(buffer));
	if (make_path(path, sizeof(path), dir, "fsync") != 0)
		return -1;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	if (now_ns(&start) != 0)
		goto out;
	for (i = 0; i < count; i++) {
		if (write_all(fd, buffer, sizeof(buffer)) != 0 ||
		    fsync(fd) != 0)
			goto out;
	}
	if (now_ns(&end) != 0)
		goto out;
	report_result(label, "write_fsync_4k", count,
	    count * sizeof(buffer), end - start);
	r = 0;
out:
	{
		int saved = errno;
		if (close(fd) != 0 && r == 0)
			return -1;
		errno = saved;
	}
	return r;
}

static void
cleanup(const char *dir)
{
	static const char * const names[] = {
		"rename-a", "rename-b", "small-write",
		"sequential-write", "fsync"
	};
	char path[PATH_MAX];
	size_t i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (make_path(path, sizeof(path), dir, names[i]) == 0)
			(void)unlink(path);
	}
	(void)rmdir(dir);
}

static int
parse_count(const char *text, uint64_t maximum, uint64_t *value)
{
	char *end;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(text, &end, 10);
	if (errno != 0 || *text == '\0' || *end != '\0' ||
	    parsed == 0 || parsed > maximum) {
		errno = EINVAL;
		return -1;
	}
	*value = (uint64_t)parsed;
	return 0;
}

int
main(int argc, char **argv)
{
	char dir[PATH_MAX];
	const char *label;
	uint64_t iterations = 200, sequential_mib = 64;
	struct stat st;
	int n;

	if (argc < 3 || argc > 5) {
		fprintf(stderr,
		    "usage: %s mountpoint label [iterations [sequential-MiB]]\n",
		    argv[0]);
		return 2;
	}
	if (strlen(argv[2]) == 0 || strchr(argv[2], ',') != NULL ||
	    strchr(argv[2], '\n') != NULL) {
		fprintf(stderr, "label must be a non-empty CSV field\n");
		return 2;
	}
	if (argc >= 4 && parse_count(argv[3], 100000, &iterations) != 0) {
		fprintf(stderr, "invalid iteration count: %s\n", argv[3]);
		return 2;
	}
	if (argc >= 5 && parse_count(argv[4], 1024, &sequential_mib) != 0) {
		fprintf(stderr, "invalid sequential MiB count: %s\n", argv[4]);
		return 2;
	}
	if (stat(argv[1], &st) != 0 || !S_ISDIR(st.st_mode)) {
		perror(argv[1]);
		return 1;
	}
	n = snprintf(dir, sizeof(dir), "%s/.mfs4-bench-%ld",
	    argv[1], (long)getpid());
	if (n < 0 || (size_t)n >= sizeof(dir)) {
		fprintf(stderr, "mountpoint path is too long\n");
		return 1;
	}
	if (mkdir(dir, 0700) != 0) {
		perror(dir);
		return 1;
	}
	label = argv[2];
	puts("format,workload,operations,bytes,elapsed_us,ops_per_sec,mib_per_sec");

	if (bench_create_unlink(dir, label, iterations) != 0 ||
	    bench_rename(dir, label, iterations) != 0 ||
	    bench_small_write(dir, label, iterations) != 0 ||
	    bench_sequential_write(dir, label, sequential_mib) != 0 ||
	    bench_fsync(dir, label, iterations) != 0) {
		perror("mfs4_fs_bench");
		cleanup(dir);
		return 1;
	}
	cleanup(dir);
	return 0;
}
