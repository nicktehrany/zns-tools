AC_PREREQ([2.68])

AC_INIT([zns-tools], [0.1], [nicktehrany1@gmail.com])

AC_PREFIX_DEFAULT(/usr)
AC_CONFIG_SRCDIR([config.h.in])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([-Wall foreign subdir-objects])
AM_SILENT_RULES([yes])

AC_CANONICAL_HOST

build_linux=no

case "${host_os}" in
    linux*)
        build_linux=yes
        ;;
    *)
        AC_MSG_ERROR(["OS $host_os is not supported"])
        ;;
esac

AM_CONDITIONAL([LINUX], [test "$build_linux" = "yes"])

AC_USE_SYSTEM_EXTENSIONS

m4_ifdef([AM_PROG_AR], [AM_PROG_AR])
m4_pattern_allow([AM_PROG_AR])
LT_INIT

# Check for programs
AC_PROG_CC
AM_PROG_CC_C_O

# Checks for header files.
AC_CHECK_HEADERS(m4_flatten([
    fcntl.h
    libgen.h
    stdint.h
    stdio.h
    linux/blkzoned.h
    linux/fiemap.h
    linux/fs.h
    linux/types.h
    sys/types.h
    inttypes.h
    stdlib.h
    string.h
    sys/ioctl.h
    sys/stat.h
    sys/syscall.h
    sys/sysmacros.h
    unistd.h
    errno.h
    sys/wait.h
]))

AC_ARG_ENABLE([multi_streams],
              AS_HELP_STRING([--enable-multi-streams],[Enable multi-streams support.]))
if test "x$enable_multi_streams" == "xyes"; then
    AC_MSG_NOTICE([Building with Multi Streams])
    AC_DEFINE(HAVE_MULTI_STREAMS, 1, [includes multi stream f2fs])
fi

# Checks blkzoned is valid for ZNS with 5.12+ Kernel
AC_CHECK_MEMBER([struct blk_zone.capacity],
		[AC_DEFINE(HAVE_BLK_ZONE_REP_V2, [1],
			   [includes zone capacity])],
		[AC_MSG_ERROR([missing zone capacity. Kernel 5.12+ required])], 
        [[#include <linux/blkzoned.h>]])

AC_CONFIG_FILES([
    Makefile
    man/Makefile
    lib/Makefile
    src/Makefile
])

AC_OUTPUT
