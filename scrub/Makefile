#
# Copyright (C) 2018 Oracle.  All Rights Reserved.
#

TOPDIR = ..
include $(TOPDIR)/include/builddefs

# On linux we get fsmap from the system or define it ourselves
# so include this based on platform type.  If this reverts to only
# the autoconf check w/o local definition, change to testing HAVE_GETFSMAP
SCRUB_PREREQS=$(PKG_PLATFORM)$(HAVE_OPENAT)$(HAVE_FSTATAT)

ifeq ($(SCRUB_PREREQS),linuxyesyes)
LTCOMMAND = xfs_scrub
INSTALL_SCRUB = install-scrub
XFS_SCRUB_ALL_PROG = xfs_scrub_all
XFS_SCRUB_ARGS = -b -n
endif	# scrub_prereqs

HFILES = \
bitmap.h \
common.h \
counter.h \
disk.h \
filemap.h \
fscounters.h \
inodes.h \
progress.h \
read_verify.h \
scrub.h \
spacemap.h \
unicrash.h \
vfs.h \
xfs_scrub.h

CFILES = \
bitmap.c \
common.c \
counter.c \
disk.c \
filemap.c \
fscounters.c \
inodes.c \
phase1.c \
phase2.c \
phase3.c \
phase4.c \
phase5.c \
phase6.c \
phase7.c \
progress.c \
read_verify.c \
scrub.c \
spacemap.c \
vfs.c \
xfs_scrub.c

LLDLIBS += $(LIBHANDLE) $(LIBFROG) $(LIBPTHREAD) $(LIBUNISTRING) $(LIBRT)
LTDEPENDENCIES += $(LIBHANDLE) $(LIBFROG) $(LIBUNISTRING) $(LIBRT)
LLDFLAGS = -static

ifeq ($(HAVE_MALLINFO),yes)
LCFLAGS += -DHAVE_MALLINFO
endif

ifeq ($(HAVE_SYNCFS),yes)
LCFLAGS += -DHAVE_SYNCFS
endif

ifeq ($(HAVE_LIBATTR),yes)
LCFLAGS += -DHAVE_LIBATTR
endif

ifeq ($(HAVE_U8NORMALIZE),yes)
CFILES += unicrash.c
LCFLAGS += -DHAVE_U8NORMALIZE
endif

ifeq ($(HAVE_SG_IO),yes)
LCFLAGS += -DHAVE_SG_IO
endif

ifeq ($(HAVE_HDIO_GETGEO),yes)
LCFLAGS += -DHAVE_HDIO_GETGEO
endif

default: depend $(LTCOMMAND) $(XFS_SCRUB_ALL_PROG)

xfs_scrub_all: xfs_scrub_all.in
	@echo "    [SED]    $@"
	$(Q)$(SED) -e "s|@sbindir@|$(PKG_ROOT_SBIN_DIR)|g" \
		   -e "s|@scrub_args@|$(XFS_SCRUB_ARGS)|g" < $< > $@
	$(Q)chmod a+x $@

phase5.o unicrash.o xfs.o: $(TOPDIR)/include/builddefs

include $(BUILDRULES)

install: $(INSTALL_SCRUB)

install-scrub: default
	$(INSTALL) -m 755 -d $(PKG_ROOT_SBIN_DIR)
	$(LTINSTALL) -m 755 $(LTCOMMAND) $(PKG_ROOT_SBIN_DIR)
	$(INSTALL) -m 755 $(XFS_SCRUB_ALL_PROG) $(PKG_ROOT_SBIN_DIR)

install-dev:

-include .dep
