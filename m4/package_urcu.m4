AC_DEFUN([AC_PACKAGE_NEED_URCU_H],
  [ AC_CHECK_HEADERS(urcu.h)
    if test $ac_cv_header_urcu_h = no; then
       AC_CHECK_HEADERS(urcu.h,, [
       echo
       echo 'FATAL ERROR: could not find a valid urcu header.'
       exit 1])
    fi
  ])

AC_DEFUN([AC_PACKAGE_NEED_RCU_INIT],
  [ AC_MSG_CHECKING([for liburcu])
    AC_TRY_COMPILE([
#define _GNU_SOURCE
#include <urcu.h>
    ], [
       rcu_init();
    ], liburcu=-lurcu
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    AC_SUBST(liburcu)
  ])

#
# Make sure that calling uatomic_inc on a 64-bit integer doesn't cause a link
# error on _uatomic_link_error, which is how liburcu signals that it doesn't
# support atomic operations on 64-bit data types.
#
AC_DEFUN([AC_HAVE_LIBURCU_ATOMIC64],
  [ AC_MSG_CHECKING([for atomic64_t support in liburcu])
    AC_TRY_LINK([
#define _GNU_SOURCE
#include <urcu.h>
    ], [
       long long f = 3;
       uatomic_inc(&f);
    ], have_liburcu_atomic64=yes
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    AC_SUBST(have_liburcu_atomic64)
  ])
