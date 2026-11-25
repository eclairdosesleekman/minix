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
	fprintf(stderr,
	    "Usage: setfacl [-d] -s acl_text file ...\n"
	    "       setfacl [-d] -b file ...\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	acl_type_t type = ACL_TYPE_ACCESS;
	const char *spec = NULL;
	int remove_acl = 0, ch, errors = 0, i;
	acl_t acl = NULL;

	while ((ch = getopt(argc, argv, "ds:b")) != -1) {
		switch (ch) {
		case 'd': type = ACL_TYPE_DEFAULT; break;
		case 's':
			if (spec != NULL || remove_acl)
				usage();
			spec = optarg;
			break;
		case 'b':
			if (spec != NULL || remove_acl)
				usage();
			remove_acl = 1;
			break;
		default: usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc == 0 || (!remove_acl && spec == NULL))
		usage();

	if (spec != NULL) {
		acl = acl_from_text(spec);
		if (acl == NULL) {
			fprintf(stderr, "setfacl: invalid ACL: %s\n",
			    strerror(errno));
			return 1;
		}
	}

	for (i = 0; i < argc; i++) {
		int r;
		if (remove_acl) {
			r = type == ACL_TYPE_DEFAULT ?
			    acl_delete_def_file(argv[i]) :
			    acl_delete_file(argv[i]);
		} else {
			r = acl_set_file(argv[i], type, acl);
		}
		if (r != 0) {
			fprintf(stderr, "setfacl: %s: %s\n", argv[i],
			    strerror(errno));
			errors = 1;
		}
	}
	if (acl != NULL)
		acl_free(acl);
	return errors;
}
