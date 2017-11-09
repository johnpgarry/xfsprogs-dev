AC_DEFUN([AC_PACKAGE_CHECK_UBSAN],
  [ AC_MSG_CHECKING([if C compiler supports UBSAN])
    OLD_CFLAGS="$CFLAGS"
    OLD_LDFLAGS="$LDFLAGS"
    UBSAN_FLAGS="-fsanitize=undefined"
    CFLAGS="$CFLAGS $UBSAN_FLAGS"
    LDFLAGS="$LDFLAGS $UBSAN_FLAGS"
    AC_LINK_IFELSE([AC_LANG_PROGRAM([])],
        [AC_MSG_RESULT([yes])]
        [ubsan_cflags=$UBSAN_FLAGS]
        [ubsan_ldflags=$UBSAN_FLAGS]
        [have_ubsan=yes],
        [AC_MSG_RESULT([no])])
    CFLAGS="${OLD_CFLAGS}"
    LDFLAGS="${OLD_LDFLAGS}"
    AC_SUBST(have_ubsan)
    AC_SUBST(ubsan_cflags)
    AC_SUBST(ubsan_ldflags)
  ])
