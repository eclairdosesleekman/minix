#include <sys/acl.h>
#include <sys/cdefs.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(void) __dead;

static void
usage(void)
{
	fprintf(stderr, "Usage: getfacl [-d] file ...\n");
	exit(1);
}

static int
print_acl(const char *path, acl_type_t type, const char *label, int optional)
{
	acl_t acl;
	char *text;
	ssize_t length;

	errno = 0;
	acl = acl_get_file(path, type);
	if (acl == NULL) {
		if (optional && (errno == ENODATA || errno == EOPNOTSUPP ||
		    errno == ENOSYS))
			return 0;
		fprintf(stderr, "getfacl: %s: %s\n", path, strerror(errno));
		return -1;
	}
	text = acl_to_text(acl, &length);
	if (text == NULL) {
		fprintf(stderr, "getfacl: %s: %s\n", path, strerror(errno));
		acl_free(acl);
		return -1;
	}
	printf("%s ACL for %s:\n", label, path);
	if (length != 0)
		(void)fwrite(text, 1, (size_t)length, stdout);
	free(text);
	acl_free(acl);
	return 0;
}

int
main(int argc, char *argv[])
{
	int ch, default_only = 0, errors = 0, i;

	while ((ch = getopt(argc, argv, "d")) != -1) {
		switch (ch) {
		case 'd': default_only = 1; break;
		default: usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc == 0)
		usage();

	for (i = 0; i < argc; i++) {
		if (!default_only &&
		    print_acl(argv[i], ACL_TYPE_ACCESS, "Access", 0) != 0)
			errors = 1;
		if (print_acl(argv[i], ACL_TYPE_DEFAULT, "Default", 1) != 0)
			errors = 1;
	}
	return errors;
}
