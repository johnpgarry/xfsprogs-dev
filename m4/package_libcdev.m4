#
# Check if we have a pwritev2 libc call (Linux)
#
AC_DEFUN([AC_HAVE_PWRITEV2],
  [ AC_MSG_CHECKING([for pwritev2])
    AC_LINK_IFELSE(
    [	AC_LANG_PROGRAM([[
#define _GNU_SOURCE
#include <sys/uio.h>
	]], [[
pwritev2(0, 0, 0, 0, 0);
	]])
    ], have_pwritev2=yes
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    AC_SUBST(have_pwritev2)
  ])

#
# Check if we have a copy_file_range system call (Linux)
#
AC_DEFUN([AC_HAVE_COPY_FILE_RANGE],
  [ AC_MSG_CHECKING([for copy_file_range])
    AC_LINK_IFELSE(
    [	AC_LANG_PROGRAM([[
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>
	]], [[
syscall(__NR_copy_file_range, 0, 0, 0, 0, 0, 0);
	]])
    ], have_copy_file_range=yes
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    AC_SUBST(have_copy_file_range)
  ])

#
# Check if we need to override the system struct fsxattr with
# the internal definition.  This /only/ happens if the system
# actually defines struct fsxattr /and/ the system definition
# is missing certain fields.
#
AC_DEFUN([AC_NEED_INTERNAL_FSXATTR],
  [
    AC_CHECK_TYPE(struct fsxattr,
      [
        AC_CHECK_MEMBER(struct fsxattr.fsx_cowextsize,
          ,
          need_internal_fsxattr=yes,
          [#include <linux/fs.h>]
        )
      ],,
      [#include <linux/fs.h>]
    )
    AC_SUBST(need_internal_fsxattr)
  ])

#
# Check if we need to override the system struct fscrypt_add_key_arg
# with the internal definition.  This /only/ happens if the system
# actually defines struct fscrypt_add_key_arg /and/ the system
# definition is missing certain fields.
#
AC_DEFUN([AC_NEED_INTERNAL_FSCRYPT_ADD_KEY_ARG],
  [
    AC_CHECK_TYPE(struct fscrypt_add_key_arg,
      [
        AC_CHECK_MEMBER(struct fscrypt_add_key_arg.key_id,
          ,
          need_internal_fscrypt_add_key_arg=yes,
          [#include <linux/fs.h>]
        )
      ],,
      [#include <linux/fs.h>]
    )
    AC_SUBST(need_internal_fscrypt_add_key_arg)
  ])

#
# Check if we need to override the system struct fscrypt_policy_v2
# with the internal definition.  This /only/ happens if the system
# actually defines struct fscrypt_policy_v2 /and/ the system
# definition is missing certain fields.
#
AC_DEFUN([AC_NEED_INTERNAL_FSCRYPT_POLICY_V2],
  [
    AC_CHECK_TYPE(struct fscrypt_policy_v2,
      [
        AC_CHECK_MEMBER(struct fscrypt_policy_v2.log2_data_unit_size,
          ,
          need_internal_fscrypt_policy_v2=yes,
          [#include <linux/fs.h>]
        )
      ],,
      [#include <linux/fs.h>]
    )
    AC_SUBST(need_internal_fscrypt_policy_v2)
  ])

#
# Check if we have a FS_IOC_GETFSMAP ioctl (Linux)
#
AC_DEFUN([AC_HAVE_GETFSMAP],
  [ AC_MSG_CHECKING([for GETFSMAP])
    AC_LINK_IFELSE(
    [	AC_LANG_PROGRAM([[
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/fs.h>
#include <linux/fsmap.h>
	]], [[
unsigned long x = FS_IOC_GETFSMAP;
struct fsmap_head fh;
	]])
    ], have_getfsmap=yes
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    AC_SUBST(have_getfsmap)
  ])

#
# Check if we have MAP_SYNC defines (Linux)
#
AC_DEFUN([AC_HAVE_MAP_SYNC],
  [ AC_MSG_CHECKING([for MAP_SYNC])
    AC_COMPILE_IFELSE(
    [	AC_LANG_PROGRAM([[
#include <sys/mman.h>
	]], [[
int flags = MAP_SYNC | MAP_SHARED_VALIDATE;
	]])
    ], have_map_sync=yes
	AC_MSG_RESULT(yes),
	AC_MSG_RESULT(no))
    AC_SUBST(have_map_sync)
  ])

#
# Check if we have a mallinfo libc call
#
AC_DEFUN([AC_HAVE_MALLINFO],
  [ AC_MSG_CHECKING([for mallinfo ])
    AC_COMPILE_IFELSE(
    [	AC_LANG_PROGRAM([[
#include <malloc.h>
	]], [[
struct mallinfo test;

test.arena = 0; test.hblkhd = 0; test.uordblks = 0; test.fordblks = 0;
test = mallinfo();
	]])
    ], have_mallinfo=yes
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    AC_SUBST(have_mallinfo)
  ])

#
# Check if we have a mallinfo2 libc call
#
AC_DEFUN([AC_HAVE_MALLINFO2],
  [ AC_MSG_CHECKING([for mallinfo2 ])
    AC_COMPILE_IFELSE(
    [	AC_LANG_PROGRAM([[
#include <malloc.h>
        ]], [[
struct mallinfo2 test;

test.arena = 0; test.hblkhd = 0; test.uordblks = 0; test.fordblks = 0;
test = mallinfo2();
        ]])
    ], have_mallinfo2=yes
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    AC_SUBST(have_mallinfo2)
  ])

AC_DEFUN([AC_PACKAGE_CHECK_LTO],
  [ AC_MSG_CHECKING([if C compiler supports LTO])
    OLD_CFLAGS="$CFLAGS"
    OLD_LDFLAGS="$LDFLAGS"
    LTO_FLAGS="-flto -ffat-lto-objects"
    CFLAGS="$CFLAGS $LTO_FLAGS"
    LDFLAGS="$LDFLAGS $LTO_FLAGS"
    AC_LINK_IFELSE([AC_LANG_PROGRAM([])],
        [AC_MSG_RESULT([yes])]
        [lto_cflags=$LTO_FLAGS]
        [lto_ldflags=$LTO_FLAGS]
        [AC_PATH_PROG(gcc_ar, gcc-ar,,)]
        [AC_PATH_PROG(gcc_ranlib, gcc-ranlib,,)],
        [AC_MSG_RESULT([no])])
    if test -x "$gcc_ar" && test -x "$gcc_ranlib"; then
        have_lto=yes
    fi
    CFLAGS="${OLD_CFLAGS}"
    LDFLAGS="${OLD_LDFLAGS}"
    AC_SUBST(gcc_ar)
    AC_SUBST(gcc_ranlib)
    AC_SUBST(have_lto)
    AC_SUBST(lto_cflags)
    AC_SUBST(lto_ldflags)
  ])

AC_DEFUN([AC_NEED_INTERNAL_STATX],
  [ AC_CHECK_TYPE(struct statx,
      [
        AC_CHECK_MEMBER(struct statx.stx_atomic_write_unit_min,
          ,
          need_internal_statx=yes,
          [#include <linux/stat.h>]
        )
      ],,
      [#include <linux/stat.h>]
    )
    AC_SUBST(need_internal_statx)
  ])
