// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016 Google, Inc.  All Rights Reserved.
 * Author: Eric Biggers <ebiggers@google.com>
 */

#include "platform_defs.h"
#include "command.h"
#include "init.h"
#include "libfrog/paths.h"
#include "io.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

/*
 * Declare the fscrypt ioctls if needed, since someone may be compiling xfsprogs
 * with old kernel headers.  But <linux/fs.h> has already been included, so be
 * careful not to declare things twice.
 */

/* first batch of ioctls (Linux headers v4.6+) */
#ifndef FS_IOC_SET_ENCRYPTION_POLICY
#define fscrypt_policy fscrypt_policy_v1
#define FS_IOC_SET_ENCRYPTION_POLICY		_IOR('f', 19, struct fscrypt_policy)
#define FS_IOC_GET_ENCRYPTION_PWSALT		_IOW('f', 20, __u8[16])
#define FS_IOC_GET_ENCRYPTION_POLICY		_IOW('f', 21, struct fscrypt_policy)
#endif

/*
 * Second batch of ioctls (Linux headers v5.4+), plus some renamings from FS_ to
 * FSCRYPT_.  We don't bother defining the old names here.
 */
#ifndef FS_IOC_GET_ENCRYPTION_POLICY_EX

#define FSCRYPT_POLICY_FLAGS_PAD_4		0x00
#define FSCRYPT_POLICY_FLAGS_PAD_8		0x01
#define FSCRYPT_POLICY_FLAGS_PAD_16		0x02
#define FSCRYPT_POLICY_FLAGS_PAD_32		0x03
#define FSCRYPT_POLICY_FLAGS_PAD_MASK		0x03
#define FSCRYPT_POLICY_FLAG_DIRECT_KEY		0x04

#define FSCRYPT_MODE_AES_256_XTS		1
#define FSCRYPT_MODE_AES_256_CTS		4
#define FSCRYPT_MODE_AES_128_CBC		5
#define FSCRYPT_MODE_AES_128_CTS		6
#define FSCRYPT_MODE_ADIANTUM			9

/*
 * In the headers for Linux v4.6 through v5.3, 'struct fscrypt_policy_v1' is
 * already defined under its old name, 'struct fscrypt_policy'.  But it's fine
 * to define it under its new name too.
 *
 * Note: "v1" policies really are version "0" in the API.
 */
#define FSCRYPT_POLICY_V1		0
#define FSCRYPT_KEY_DESCRIPTOR_SIZE	8
struct fscrypt_policy_v1 {
	__u8 version;
	__u8 contents_encryption_mode;
	__u8 filenames_encryption_mode;
	__u8 flags;
	__u8 master_key_descriptor[FSCRYPT_KEY_DESCRIPTOR_SIZE];
};

#define FSCRYPT_POLICY_V2		2
#define FSCRYPT_KEY_IDENTIFIER_SIZE	16
struct fscrypt_policy_v2 {
	__u8 version;
	__u8 contents_encryption_mode;
	__u8 filenames_encryption_mode;
	__u8 flags;
	__u8 __reserved[4];
	__u8 master_key_identifier[FSCRYPT_KEY_IDENTIFIER_SIZE];
};

#define FSCRYPT_MAX_KEY_SIZE		64

#define FS_IOC_GET_ENCRYPTION_POLICY_EX		_IOWR('f', 22, __u8[9]) /* size + version */
struct fscrypt_get_policy_ex_arg {
	__u64 policy_size; /* input/output */
	union {
		__u8 version;
		struct fscrypt_policy_v1 v1;
		struct fscrypt_policy_v2 v2;
	} policy; /* output */
};

#define FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR	1
#define FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER	2
struct fscrypt_key_specifier {
	__u32 type;	/* one of FSCRYPT_KEY_SPEC_TYPE_* */
	__u32 __reserved;
	union {
		__u8 __reserved[32]; /* reserve some extra space */
		__u8 descriptor[FSCRYPT_KEY_DESCRIPTOR_SIZE];
		__u8 identifier[FSCRYPT_KEY_IDENTIFIER_SIZE];
	} u;
};

#define FS_IOC_ADD_ENCRYPTION_KEY		_IOWR('f', 23, struct fscrypt_add_key_arg)
struct fscrypt_add_key_arg {
	struct fscrypt_key_specifier key_spec;
	__u32 raw_size;
	__u32 __reserved[9];
	__u8 raw[];
};

#define FS_IOC_REMOVE_ENCRYPTION_KEY		_IOWR('f', 24, struct fscrypt_remove_key_arg)
#define FS_IOC_REMOVE_ENCRYPTION_KEY_ALL_USERS	_IOWR('f', 25, struct fscrypt_remove_key_arg)
struct fscrypt_remove_key_arg {
	struct fscrypt_key_specifier key_spec;
#define FSCRYPT_KEY_REMOVAL_STATUS_FLAG_FILES_BUSY	0x00000001
#define FSCRYPT_KEY_REMOVAL_STATUS_FLAG_OTHER_USERS	0x00000002
	__u32 removal_status_flags;	/* output */
	__u32 __reserved[5];
};

#define FS_IOC_GET_ENCRYPTION_KEY_STATUS	_IOWR('f', 26, struct fscrypt_get_key_status_arg)
struct fscrypt_get_key_status_arg {
	/* input */
	struct fscrypt_key_specifier key_spec;
	__u32 __reserved[6];

	/* output */
#define FSCRYPT_KEY_STATUS_ABSENT		1
#define FSCRYPT_KEY_STATUS_PRESENT		2
#define FSCRYPT_KEY_STATUS_INCOMPLETELY_REMOVED	3
	__u32 status;
#define FSCRYPT_KEY_STATUS_FLAG_ADDED_BY_SELF   0x00000001
	__u32 status_flags;
	__u32 user_count;
	__u32 __out_reserved[13];
};

#endif /* !FS_IOC_GET_ENCRYPTION_POLICY_EX */

static const struct {
	__u8 mode;
	const char *name;
} available_modes[] = {
	{FSCRYPT_MODE_AES_256_XTS, "AES-256-XTS"},
	{FSCRYPT_MODE_AES_256_CTS, "AES-256-CTS"},
};

static cmdinfo_t get_encpolicy_cmd;
static cmdinfo_t set_encpolicy_cmd;

static void
set_encpolicy_help(void)
{
	int i;

	printf(_(
"\n"
" assign an encryption policy to the currently open file\n"
"\n"
" Examples:\n"
" 'set_encpolicy' - assign policy with default key [0000000000000000]\n"
" 'set_encpolicy 0000111122223333' - assign policy with specified key\n"
"\n"
" -c MODE -- contents encryption mode\n"
" -n MODE -- filenames encryption mode\n"
" -f FLAGS -- policy flags\n"
" -v VERSION -- version of policy structure\n"
"\n"
" MODE can be numeric or one of the following predefined values:\n"));
	printf("    ");
	for (i = 0; i < ARRAY_SIZE(available_modes); i++) {
		printf("%s", available_modes[i].name);
		if (i != ARRAY_SIZE(available_modes) - 1)
			printf(", ");
	}
	printf("\n");
	printf(_(
" FLAGS and VERSION must be numeric.\n"
"\n"
" Note that it's only possible to set an encryption policy on an empty\n"
" directory.  It's then inherited by new files and subdirectories.\n"
"\n"));
}

static bool
parse_byte_value(const char *arg, __u8 *value_ret)
{
	long value;
	char *tmp;

	value = strtol(arg, &tmp, 0);
	if (value < 0 || value > 255 || tmp == arg || *tmp != '\0')
		return false;
	*value_ret = value;
	return true;
}

static bool
parse_mode(const char *arg, __u8 *mode_ret)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(available_modes); i++) {
		if (strcmp(arg, available_modes[i].name) == 0) {
			*mode_ret = available_modes[i].mode;
			return true;
		}
	}

	return parse_byte_value(arg, mode_ret);
}

static const char *
mode2str(__u8 mode)
{
	static char buf[32];
	int i;

	for (i = 0; i < ARRAY_SIZE(available_modes); i++)
		if (mode == available_modes[i].mode)
			return available_modes[i].name;

	sprintf(buf, "0x%02x", mode);
	return buf;
}

static const char *
keydesc2str(__u8 master_key_descriptor[FSCRYPT_KEY_DESCRIPTOR_SIZE])
{
	static char buf[2 * FSCRYPT_KEY_DESCRIPTOR_SIZE + 1];
	int i;

	for (i = 0; i < FSCRYPT_KEY_DESCRIPTOR_SIZE; i++)
		sprintf(&buf[2 * i], "%02x", master_key_descriptor[i]);

	return buf;
}

static int
get_encpolicy_f(int argc, char **argv)
{
	struct fscrypt_policy policy;

	if (ioctl(file->fd, FS_IOC_GET_ENCRYPTION_POLICY, &policy) < 0) {
		fprintf(stderr, "%s: failed to get encryption policy: %s\n",
			file->name, strerror(errno));
		exitcode = 1;
		return 0;
	}

	printf("Encryption policy for %s:\n", file->name);
	printf("\tPolicy version: %u\n", policy.version);
	printf("\tMaster key descriptor: %s\n",
	       keydesc2str(policy.master_key_descriptor));
	printf("\tContents encryption mode: %u (%s)\n",
	       policy.contents_encryption_mode,
	       mode2str(policy.contents_encryption_mode));
	printf("\tFilenames encryption mode: %u (%s)\n",
	       policy.filenames_encryption_mode,
	       mode2str(policy.filenames_encryption_mode));
	printf("\tFlags: 0x%02x\n", policy.flags);
	return 0;
}

static int
set_encpolicy_f(int argc, char **argv)
{
	int c;
	struct fscrypt_policy policy;

	/* Initialize the policy structure with default values */
	memset(&policy, 0, sizeof(policy));
	policy.contents_encryption_mode = FSCRYPT_MODE_AES_256_XTS;
	policy.filenames_encryption_mode = FSCRYPT_MODE_AES_256_CTS;
	policy.flags = FSCRYPT_POLICY_FLAGS_PAD_16;

	/* Parse options */
	while ((c = getopt(argc, argv, "c:n:f:v:")) != EOF) {
		switch (c) {
		case 'c':
			if (!parse_mode(optarg,
					&policy.contents_encryption_mode)) {
				fprintf(stderr, "invalid contents encryption "
					"mode: %s\n", optarg);
				return 0;
			}
			break;
		case 'n':
			if (!parse_mode(optarg,
					&policy.filenames_encryption_mode)) {
				fprintf(stderr, "invalid filenames encryption "
					"mode: %s\n", optarg);
				return 0;
			}
			break;
		case 'f':
			if (!parse_byte_value(optarg, &policy.flags)) {
				fprintf(stderr, "invalid flags: %s\n", optarg);
				return 0;
			}
			break;
		case 'v':
			if (!parse_byte_value(optarg, &policy.version)) {
				fprintf(stderr, "invalid policy version: %s\n",
					optarg);
				return 0;
			}
			break;
		default:
			return command_usage(&set_encpolicy_cmd);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 1)
		return command_usage(&set_encpolicy_cmd);

	/* Parse key descriptor if specified */
	if (argc > 0) {
		const char *keydesc = argv[0];
		char *tmp;
		unsigned long long x;
		int i;

		if (strlen(keydesc) != FSCRYPT_KEY_DESCRIPTOR_SIZE * 2) {
			fprintf(stderr, "invalid key descriptor: %s\n",
				keydesc);
			return 0;
		}

		x = strtoull(keydesc, &tmp, 16);
		if (tmp == keydesc || *tmp != '\0') {
			fprintf(stderr, "invalid key descriptor: %s\n",
				keydesc);
			return 0;
		}

		for (i = 0; i < FSCRYPT_KEY_DESCRIPTOR_SIZE; i++) {
			policy.master_key_descriptor[i] = x >> 56;
			x <<= 8;
		}
	}

	/* Set the encryption policy */
	if (ioctl(file->fd, FS_IOC_SET_ENCRYPTION_POLICY, &policy) < 0) {
		fprintf(stderr, "%s: failed to set encryption policy: %s\n",
			file->name, strerror(errno));
		exitcode = 1;
		return 0;
	}

	return 0;
}

void
encrypt_init(void)
{
	get_encpolicy_cmd.name = "get_encpolicy";
	get_encpolicy_cmd.cfunc = get_encpolicy_f;
	get_encpolicy_cmd.argmin = 0;
	get_encpolicy_cmd.argmax = 0;
	get_encpolicy_cmd.flags = CMD_NOMAP_OK | CMD_FOREIGN_OK;
	get_encpolicy_cmd.oneline =
		_("display the encryption policy of the current file");

	set_encpolicy_cmd.name = "set_encpolicy";
	set_encpolicy_cmd.cfunc = set_encpolicy_f;
	set_encpolicy_cmd.args =
		_("[-c mode] [-n mode] [-f flags] [-v version] [keydesc]");
	set_encpolicy_cmd.argmin = 0;
	set_encpolicy_cmd.argmax = -1;
	set_encpolicy_cmd.flags = CMD_NOMAP_OK | CMD_FOREIGN_OK;
	set_encpolicy_cmd.oneline =
		_("assign an encryption policy to the current file");
	set_encpolicy_cmd.help = set_encpolicy_help;

	add_command(&get_encpolicy_cmd);
	add_command(&set_encpolicy_cmd);
}
