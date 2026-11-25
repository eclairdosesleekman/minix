/*
 * POSIX.1e access control list interface for MINIX.
 *
 * ACL values are stored as the MFS4 version-2 little-endian xattr payload.
 */
#ifndef _SYS_ACL_H_
#define _SYS_ACL_H_

#include <sys/types.h>
#include <stdint.h>
#include <sys/cdefs.h>

typedef struct _acl *acl_t;
typedef struct _acl_entry *acl_entry_t;
typedef uint16_t *acl_permset_t;
typedef int acl_tag_t;
typedef int acl_type_t;

#define ACL_UNDEFINED_ID ((uint32_t)0xffffffffU)

#define ACL_USER_OBJ  0x0001
#define ACL_USER      0x0002
#define ACL_GROUP_OBJ 0x0004
#define ACL_GROUP     0x0008
#define ACL_MASK      0x0010
#define ACL_OTHER     0x0020

#define ACL_READ      0x04
#define ACL_WRITE     0x02
#define ACL_EXECUTE   0x01

#define ACL_FIRST_ENTRY 0
#define ACL_NEXT_ENTRY  1

#define ACL_TYPE_ACCESS  0
#define ACL_TYPE_DEFAULT 1

#define ACL_MAX_ENTRIES 474

__BEGIN_DECLS
acl_t acl_init(int);
acl_t acl_dup(acl_t);
int acl_free(void *);
int acl_valid(acl_t);
int acl_cmp(acl_t, acl_t);
int acl_calc_mask(acl_t *);

int acl_create_entry(acl_t *, acl_entry_t *);
int acl_delete_entry(acl_t, acl_entry_t);
int acl_get_entry(acl_t, int, acl_entry_t *);
int acl_get_tag_type(acl_entry_t, acl_tag_t *);
int acl_set_tag_type(acl_entry_t, acl_tag_t);
void *acl_get_qualifier(acl_entry_t);
int acl_set_qualifier(acl_entry_t, const void *);
int acl_get_permset(acl_entry_t, acl_permset_t *);
int acl_set_permset(acl_entry_t, const acl_permset_t);
int acl_add_perm(acl_permset_t, int);
int acl_delete_perm(acl_permset_t, int);
int acl_clear_perms(acl_permset_t);
int acl_get_perm(acl_permset_t, int);

acl_t acl_get_file(const char *, acl_type_t);
acl_t acl_get_link_np(const char *, acl_type_t);
acl_t acl_get_fd(int, acl_type_t);
int acl_set_file(const char *, acl_type_t, acl_t);
int acl_set_link_np(const char *, acl_type_t, acl_t);
int acl_set_fd(int, acl_type_t, acl_t);
int acl_delete_file(const char *);
int acl_delete_def_file(const char *);

acl_t acl_from_text(const char *);
char *acl_to_text(acl_t, ssize_t *);
__END_DECLS

#endif /* _SYS_ACL_H_ */
