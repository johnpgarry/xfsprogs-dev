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
