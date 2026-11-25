#include <sys/cdefs.h>
#include <sys/acl.h>
#include <sys/xattr.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <minix/posix_acl.h>

#define ACL_OBJECT_MAGIC 0x41434c32U
#define ACL_ACCESS_NAME "system.posix_acl_access"
#define ACL_DEFAULT_NAME "system.posix_acl_default"

struct _acl_entry {
	uint16_t tag;
	uint16_t perm;
	uint32_t id;
};

struct _acl {
	uint32_t magic;
	size_t count;
	size_t capacity;
	size_t iterator;
	struct _acl_entry *entries;
};

static int
acl_error(int error)
{
	errno = error;
	return -1;
}

static int
acl_type_name(acl_type_t type, const char **name)
{
	if (type == ACL_TYPE_ACCESS)
		*name = ACL_ACCESS_NAME;
	else if (type == ACL_TYPE_DEFAULT)
		*name = ACL_DEFAULT_NAME;
	else
		return EINVAL;
	return 0;
}

static int
acl_is_object(acl_t acl)
{
	return acl != NULL && acl->magic == ACL_OBJECT_MAGIC;
}

acl_t
acl_init(int count)
{
	acl_t acl;

	if (count < 0 || count > ACL_MAX_ENTRIES) {
		errno = EINVAL;
		return NULL;
	}
	acl = calloc(1, sizeof(*acl));
	if (acl == NULL)
		return NULL;
	acl->magic = ACL_OBJECT_MAGIC;
	acl->capacity = count > 0 ? (size_t)count : 4;
	acl->entries = calloc(acl->capacity, sizeof(*acl->entries));
	if (acl->entries == NULL) {
		free(acl);
		return NULL;
	}
	return acl;
}

int
acl_free(void *object)
{
	acl_t acl = object;

	if (object == NULL)
		return 0;
	if (acl_is_object(acl)) {
		acl->magic = 0;
		free(acl->entries);
		free(acl);
	} else {
		free(object);
	}
	return 0;
}

acl_t
acl_dup(acl_t source)
{
	acl_t copy;

	if (!acl_is_object(source)) {
		acl_error(EINVAL);
		return NULL;
	}
	copy = acl_init((int)source->count);
	if (copy == NULL)
		return NULL;
	if (source->count != 0)
		memcpy(copy->entries, source->entries,
		    source->count * sizeof(*source->entries));
	copy->count = source->count;
	return copy;
}

int
acl_create_entry(acl_t *acl_p, acl_entry_t *entry_p)
{
	acl_t acl;
	struct _acl_entry *entries;
	size_t capacity;

	if (acl_p == NULL || entry_p == NULL || !acl_is_object(*acl_p))
		return acl_error(EINVAL);
	acl = *acl_p;
	if (acl->count >= ACL_MAX_ENTRIES)
		return acl_error(E2BIG);
	if (acl->count == acl->capacity) {
		capacity = acl->capacity * 2;
		if (capacity > ACL_MAX_ENTRIES)
			capacity = ACL_MAX_ENTRIES;
		entries = realloc(acl->entries, capacity * sizeof(*entries));
		if (entries == NULL)
			return -1;
		memset(entries + acl->capacity, 0,
		    (capacity - acl->capacity) * sizeof(*entries));
		acl->entries = entries;
		acl->capacity = capacity;
	}
	*entry_p = &acl->entries[acl->count++];
	(*entry_p)->tag = 0;
	(*entry_p)->perm = 0;
	(*entry_p)->id = ACL_UNDEFINED_ID;
	return 0;
}

int
acl_delete_entry(acl_t acl, acl_entry_t entry)
{
	size_t index;

	if (!acl_is_object(acl) || entry == NULL ||
	    entry < acl->entries || entry >= acl->entries + acl->count)
		return acl_error(EINVAL);
	index = (size_t)(entry - acl->entries);
	if (index + 1 < acl->count)
		memmove(&acl->entries[index], &acl->entries[index + 1],
		    (acl->count - index - 1) * sizeof(*acl->entries));
	acl->count--;
	memset(&acl->entries[acl->count], 0, sizeof(*acl->entries));
	acl->entries[acl->count].id = ACL_UNDEFINED_ID;
	acl->iterator = 0;
	return 0;
}

int
acl_get_entry(acl_t acl, int entry_id, acl_entry_t *entry_p)
{
	if (!acl_is_object(acl) || entry_p == NULL ||
	    (entry_id != ACL_FIRST_ENTRY && entry_id != ACL_NEXT_ENTRY))
		return acl_error(EINVAL);
	if (entry_id == ACL_FIRST_ENTRY)
		acl->iterator = 0;
	else if (acl->iterator < acl->count)
		acl->iterator++;
	if (acl->iterator >= acl->count) {
		*entry_p = NULL;
		return 0;
	}
	*entry_p = &acl->entries[acl->iterator];
	return 1;
}

int
acl_get_tag_type(acl_entry_t entry, acl_tag_t *tag)
{
	if (entry == NULL || tag == NULL)
		return acl_error(EINVAL);
	*tag = entry->tag;
	return 0;
}

int
acl_set_tag_type(acl_entry_t entry, acl_tag_t tag)
{
	if (entry == NULL || (tag != ACL_USER_OBJ && tag != ACL_USER &&
	    tag != ACL_GROUP_OBJ && tag != ACL_GROUP &&
	    tag != ACL_MASK && tag != ACL_OTHER))
		return acl_error(EINVAL);
	entry->tag = (uint16_t)tag;
	if (tag != ACL_USER && tag != ACL_GROUP)
		entry->id = ACL_UNDEFINED_ID;
	else if (entry->id == ACL_UNDEFINED_ID)
		entry->id = 0;
	return 0;
}

void *
acl_get_qualifier(acl_entry_t entry)
{
	void *qualifier;

	if (entry == NULL) {
		errno = EINVAL;
		return NULL;
	}
	if (entry->tag == ACL_USER) {
		uid_t *uid = malloc(sizeof(*uid));
		if (uid == NULL)
			return NULL;
		*uid = (uid_t)entry->id;
		qualifier = uid;
	} else if (entry->tag == ACL_GROUP) {
		gid_t *gid = malloc(sizeof(*gid));
		if (gid == NULL)
			return NULL;
		*gid = (gid_t)entry->id;
		qualifier = gid;
	} else {
		errno = EINVAL;
		return NULL;
	}
	return qualifier;
}

int
acl_set_qualifier(acl_entry_t entry, const void *qualifier)
{
	if (entry == NULL || qualifier == NULL)
		return acl_error(EINVAL);
	if (entry->tag == ACL_USER)
		entry->id = (uint32_t)*(const uid_t *)qualifier;
	else if (entry->tag == ACL_GROUP)
		entry->id = (uint32_t)*(const gid_t *)qualifier;
	else
		return acl_error(EINVAL);
	if (entry->id == ACL_UNDEFINED_ID)
		return acl_error(EINVAL);
	return 0;
}

int
acl_get_permset(acl_entry_t entry, acl_permset_t *permset)
{
	if (entry == NULL || permset == NULL)
		return acl_error(EINVAL);
	*permset = &entry->perm;
	return 0;
}

int
acl_set_permset(acl_entry_t entry, const acl_permset_t permset)
{
	if (entry == NULL || permset == NULL ||
	    (*permset & ~MINIX_ACL_PERM_MASK) != 0)
		return acl_error(EINVAL);
	entry->perm = *permset;
	return 0;
}

int
acl_add_perm(acl_permset_t permset, int perm)
{
	if (permset == NULL || perm == 0 ||
	    (perm & ~(ACL_READ | ACL_WRITE | ACL_EXECUTE)) != 0)
		return acl_error(EINVAL);
	*permset |= (uint16_t)perm;
	return 0;
}

int
acl_delete_perm(acl_permset_t permset, int perm)
{
	if (permset == NULL || perm == 0 ||
	    (perm & ~(ACL_READ | ACL_WRITE | ACL_EXECUTE)) != 0)
		return acl_error(EINVAL);
	*permset &= (uint16_t)~perm;
	return 0;
}

int
acl_clear_perms(acl_permset_t permset)
{
	if (permset == NULL)
		return acl_error(EINVAL);
	*permset = 0;
	return 0;
}

int
acl_get_perm(acl_permset_t permset, int perm)
{
	if (permset == NULL || perm == 0 ||
	    (perm & ~(ACL_READ | ACL_WRITE | ACL_EXECUTE)) != 0)
		return acl_error(EINVAL);
	return ((*permset & perm) == perm) ? 1 : 0;
}

int
acl_valid(acl_t acl)
{
	uint8_t wire[MINIX_ACL_HEADER_SIZE +
	    ACL_MAX_ENTRIES * MINIX_ACL_ENTRY_SIZE];
	size_t i, length;

	if (!acl_is_object(acl) || acl->count > ACL_MAX_ENTRIES)
		return acl_error(EINVAL);
	length = MINIX_ACL_HEADER_SIZE +
	    acl->count * MINIX_ACL_ENTRY_SIZE;
	memset(wire, 0, length);
	minix_acl_put_le32(wire, MINIX_ACL_VERSION);
	minix_acl_put_le32(wire + 4, (uint32_t)acl->count);
	for (i = 0; i < acl->count; i++) {
		size_t off = minix_acl_entry_offset(i);
		minix_acl_put_le16(wire + off, acl->entries[i].tag);
		minix_acl_put_le16(wire + off + 2, acl->entries[i].perm);
		minix_acl_put_le32(wire + off + 4, acl->entries[i].id);
	}
	if (minix_acl_validate(wire, length) != 0)
		return acl_error(EINVAL);
	return 0;
}

int
acl_cmp(acl_t a, acl_t b)
{
	if (!acl_is_object(a) || !acl_is_object(b))
		return acl_error(EINVAL);
	if (a->count != b->count)
		return 1;
	return memcmp(a->entries, b->entries,
	    a->count * sizeof(*a->entries)) == 0 ? 0 : 1;
}

int
acl_calc_mask(acl_t *acl_p)
{
	acl_t acl;
	acl_entry_t entry;
	size_t i;
	uint16_t mask = 0;
	int have_mask = 0;

	if (acl_p == NULL || !acl_is_object(*acl_p))
		return acl_error(EINVAL);
	acl = *acl_p;
	for (i = 0; i < acl->count; i++) {
		entry = &acl->entries[i];
		if (entry->tag == ACL_GROUP_OBJ || entry->tag == ACL_USER ||
		    entry->tag == ACL_GROUP)
			mask |= entry->perm;
		if (entry->tag == ACL_MASK)
			have_mask = 1;
	}
	if (have_mask) {
		for (i = 0; i < acl->count; i++)
			if (acl->entries[i].tag == ACL_MASK)
				acl->entries[i].perm = mask;
	} else {
		if (acl_create_entry(acl_p, &entry) != 0)
			return -1;
		entry->tag = ACL_MASK;
		entry->id = ACL_UNDEFINED_ID;
		entry->perm = mask;
	}
	return 0;
}

static acl_t
acl_unpack(const uint8_t *wire, size_t length)
{
	acl_t acl;
	uint32_t count;
	size_t i;

	if (minix_acl_validate(wire, length) != 0) {
		errno = EIO;
		return NULL;
	}
	count = minix_acl_get_le32(wire + 4);
	acl = acl_init((int)count);
	if (acl == NULL)
		return NULL;
	for (i = 0; i < count; i++) {
		size_t off = minix_acl_entry_offset(i);
		acl->entries[i].tag = minix_acl_get_le16(wire + off);
		acl->entries[i].perm = minix_acl_get_le16(wire + off + 2);
		acl->entries[i].id = minix_acl_get_le32(wire + off + 4);
	}
	acl->count = count;
	return acl;
}

static int
acl_pack(acl_t acl, uint8_t **wire, size_t *length)
{
	uint8_t *p;
	size_t i, size;

	if (acl_valid(acl) != 0)
		return -1;
	size = MINIX_ACL_HEADER_SIZE +
	    acl->count * MINIX_ACL_ENTRY_SIZE;
	p = calloc(1, size);
	if (p == NULL)
		return -1;
	minix_acl_put_le32(p, MINIX_ACL_VERSION);
	minix_acl_put_le32(p + 4, (uint32_t)acl->count);
	for (i = 0; i < acl->count; i++) {
		size_t off = minix_acl_entry_offset(i);
		minix_acl_put_le16(p + off, acl->entries[i].tag);
		minix_acl_put_le16(p + off + 2, acl->entries[i].perm);
		minix_acl_put_le32(p + off + 4, acl->entries[i].id);
	}
	*wire = p;
	*length = size;
	return 0;
}

static acl_t
acl_from_mode(mode_t mode)
{
	acl_t acl;
	acl_entry_t entry;
	static const acl_tag_t tags[] = {
		ACL_USER_OBJ, ACL_GROUP_OBJ, ACL_OTHER
	};
	static const unsigned int shifts[] = { 6, 3, 0 };
	size_t i;

	acl = acl_init(3);
	if (acl == NULL)
		return NULL;
	for (i = 0; i < 3; i++) {
		if (acl_create_entry(&acl, &entry) != 0) {
			acl_free(acl);
			return NULL;
		}
		entry->tag = (uint16_t)tags[i];
		entry->id = ACL_UNDEFINED_ID;
		entry->perm = (uint16_t)((mode >> shifts[i]) & 7U);
	}
	return acl;
}

static acl_t
acl_get_xattr(const char *path, int fd, int nofollow, acl_type_t type)
{
	const char *name;
	uint8_t *wire;
	ssize_t length, actual;
	acl_t acl;

	if (fd < 0 && path == NULL) {
		errno = EINVAL;
		return NULL;
	}
	if (acl_type_name(type, &name) != 0) {
		errno = EINVAL;
		return NULL;
	}
	if (fd >= 0)
		length = fgetxattr(fd, name, NULL, 0);
	else if (nofollow)
		length = lgetxattr(path, name, NULL, 0);
	else
		length = getxattr(path, name, NULL, 0);
	if (length < 0) {
		if (type != ACL_TYPE_ACCESS ||
		    (errno != ENODATA && errno != EOPNOTSUPP &&
		    errno != ENOSYS))
			return NULL;
		{
			struct stat st;
			int sr = fd >= 0 ? fstat(fd, &st) :
			    (nofollow ? lstat(path, &st) : stat(path, &st));
			if (sr != 0)
				return NULL;
			return acl_from_mode(st.st_mode);
		}
	}
	if (length < (ssize_t)MINIX_ACL_HEADER_SIZE ||
	    (size_t)length > XATTR_SIZE_MAX) {
		errno = EIO;
		return NULL;
	}
	wire = malloc((size_t)length);
	if (wire == NULL)
		return NULL;
	if (fd >= 0)
		actual = fgetxattr(fd, name, wire, (size_t)length);
	else if (nofollow)
		actual = lgetxattr(path, name, wire, (size_t)length);
	else
		actual = getxattr(path, name, wire, (size_t)length);
	if (actual < 0 || actual != length) {
		if (actual >= 0)
			errno = EIO;
		free(wire);
		return NULL;
	}
	acl = acl_unpack(wire, (size_t)length);
	free(wire);
	return acl;
}

acl_t
acl_get_file(const char *path, acl_type_t type)
{
	return acl_get_xattr(path, -1, 0, type);
}

acl_t
acl_get_link_np(const char *path, acl_type_t type)
{
	return acl_get_xattr(path, -1, 1, type);
}

acl_t
acl_get_fd(int fd, acl_type_t type)
{
	if (fd < 0) {
		errno = EBADF;
		return NULL;
	}
	return acl_get_xattr(NULL, fd, 0, type);
}

static int
acl_set_xattr(const char *path, int fd, int nofollow, acl_type_t type,
    acl_t acl)
{
	const char *name;
	uint8_t *wire;
	size_t length;
	int r;

	if (acl_type_name(type, &name) != 0)
		return acl_error(EINVAL);
	if (acl_pack(acl, &wire, &length) != 0)
		return -1;
	if (fd >= 0)
		r = fsetxattr(fd, name, wire, length, 0);
	else if (nofollow)
		r = lsetxattr(path, name, wire, length, 0);
	else
		r = setxattr(path, name, wire, length, 0);
	free(wire);
	return r;
}

int
acl_set_file(const char *path, acl_type_t type, acl_t acl)
{
	return acl_set_xattr(path, -1, 0, type, acl);
}

int
acl_set_link_np(const char *path, acl_type_t type, acl_t acl)
{
	return acl_set_xattr(path, -1, 1, type, acl);
}

int
acl_set_fd(int fd, acl_type_t type, acl_t acl)
{
	if (fd < 0)
		return acl_error(EBADF);
	return acl_set_xattr(NULL, fd, 0, type, acl);
}

int
acl_delete_file(const char *path)
{
	return removexattr(path, ACL_ACCESS_NAME);
}

int
acl_delete_def_file(const char *path)
{
	return removexattr(path, ACL_DEFAULT_NAME);
}

static int
acl_parse_permissions(const char *text, uint16_t *permissions)
{
	uint16_t p = 0;

	if (text == NULL || strlen(text) != 3)
		return EINVAL;
	if (text[0] == 'r') p |= ACL_READ;
	else if (text[0] != '-') return EINVAL;
	if (text[1] == 'w') p |= ACL_WRITE;
	else if (text[1] != '-') return EINVAL;
	if (text[2] == 'x') p |= ACL_EXECUTE;
	else if (text[2] != '-') return EINVAL;
	*permissions = p;
	return 0;
}

static int
acl_parse_id(const char *text, int is_user, uint32_t *id)
{
	char *end;
	unsigned long number;

	if (text == NULL || *text == '\0')
		return EINVAL;
	errno = 0;
	number = strtoul(text, &end, 10);
	if (errno == 0 && *end == '\0' && number < UINT32_MAX) {
		*id = (uint32_t)number;
		return 0;
	}
	if (is_user) {
		struct passwd *pw = getpwnam(text);
		if (pw != NULL) {
			*id = (uint32_t)pw->pw_uid;
			return *id == UINT32_MAX ? EINVAL : 0;
		}
	} else {
		struct group *gr = getgrnam(text);
		if (gr != NULL) {
			*id = (uint32_t)gr->gr_gid;
			return *id == UINT32_MAX ? EINVAL : 0;
		}
	}
	return EINVAL;
}

acl_t
acl_from_text(const char *text)
{
	char *copy, *save = NULL, *line;
	acl_t acl;
	int r;

	if (text == NULL) {
		errno = EINVAL;
		return NULL;
	}
	copy = strdup(text);
	if (copy == NULL)
		return NULL;
	acl = acl_init(6);
	if (acl == NULL) {
		free(copy);
		return NULL;
	}
	for (line = strtok_r(copy, "\n", &save); line != NULL;
	    line = strtok_r(NULL, "\n", &save)) {
		char *first, *second, *third;
		acl_entry_t entry;
		uint32_t id = ACL_UNDEFINED_ID;
		uint16_t perm;
		acl_tag_t tag;
		size_t n;

		while (*line == ' ' || *line == '\t')
			line++;
		n = strlen(line);
		while (n != 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' ||
		    line[n - 1] == '\r'))
			line[--n] = '\0';
		if (*line == '\0' || *line == '#')
			continue;
		first = strchr(line, ':');
		if (first == NULL) { r = EINVAL; goto fail; }
		*first++ = '\0';
		second = strchr(first, ':');
		if (second == NULL) { r = EINVAL; goto fail; }
		*second++ = '\0';
		third = strchr(second, ':');
		if (third != NULL) { r = EINVAL; goto fail; }

		if (strcmp(line, "user") == 0) {
			if (*first == '\0')
				tag = ACL_USER_OBJ;
			else {
				tag = ACL_USER;
				r = acl_parse_id(first, 1, &id);
				if (r != 0) goto fail;
			}
		} else if (strcmp(line, "group") == 0) {
			if (*first == '\0')
				tag = ACL_GROUP_OBJ;
			else {
				tag = ACL_GROUP;
				r = acl_parse_id(first, 0, &id);
				if (r != 0) goto fail;
			}
		} else if (strcmp(line, "mask") == 0 && *first == '\0') {
			tag = ACL_MASK;
		} else if (strcmp(line, "other") == 0 && *first == '\0') {
			tag = ACL_OTHER;
		} else {
			r = EINVAL;
			goto fail;
		}
		r = acl_parse_permissions(second, &perm);
		if (r != 0) goto fail;
		if (acl_create_entry(&acl, &entry) != 0) {
			r = errno;
			goto fail;
		}
		entry->tag = (uint16_t)tag;
		entry->id = id;
		entry->perm = perm;
	}
	free(copy);
	if (acl_valid(acl) != 0) {
		acl_free(acl);
		errno = EINVAL;
		return NULL;
	}
	return acl;

fail:
	free(copy);
	acl_free(acl);
	errno = r;
	return NULL;
}

char *
acl_to_text(acl_t acl, ssize_t *length_p)
{
	char *text, line[128];
	size_t capacity, used = 0, i;
	int n;

	if (!acl_is_object(acl) || acl_valid(acl) != 0)
		return NULL;
	capacity = acl->count * sizeof(line) + 1;
	text = malloc(capacity);
	if (text == NULL)
		return NULL;
	for (i = 0; i < acl->count; i++) {
		const char *kind, *qualifier = "";
		char idbuf[24];
		struct _acl_entry *entry = &acl->entries[i];
		uint16_t p = entry->perm;

		switch (entry->tag) {
		case ACL_USER_OBJ: kind = "user"; break;
		case ACL_USER:
			kind = "user";
			(void)snprintf(idbuf, sizeof(idbuf), "%u", entry->id);
			qualifier = idbuf;
			break;
		case ACL_GROUP_OBJ: kind = "group"; break;
		case ACL_GROUP:
			kind = "group";
			(void)snprintf(idbuf, sizeof(idbuf), "%u", entry->id);
			qualifier = idbuf;
			break;
		case ACL_MASK: kind = "mask"; break;
		case ACL_OTHER: kind = "other"; break;
		default: free(text); errno = EINVAL; return NULL;
		}
		n = snprintf(line, sizeof(line), "%s:%s:%c%c%c\n", kind,
		    qualifier, (p & ACL_READ) ? 'r' : '-',
		    (p & ACL_WRITE) ? 'w' : '-',
		    (p & ACL_EXECUTE) ? 'x' : '-');
		if (n < 0 || (size_t)n >= sizeof(line) ||
		    (size_t)n > capacity - used - 1) {
			free(text);
			errno = EOVERFLOW;
			return NULL;
		}
		memcpy(text + used, line, (size_t)n);
		used += (size_t)n;
	}
	text[used] = '\0';
	if (length_p != NULL)
		*length_p = (ssize_t)used;
	return text;
}
