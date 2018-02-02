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
endif	# scrub_prereqs

HFILES = \
bitmap.h \
common.h \
counter.h \
disk.h \
filemap.h \
fscounters.h \
inodes.h \
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
phase5.c \
phase6.c \
read_verify.c \
scrub.c \
spacemap.c \
vfs.c \
xfs_scrub.c

LLDLIBS += $(LIBHANDLE) $(LIBFROG) $(LIBPTHREAD) $(LIBUNISTRING)
LTDEPENDENCIES += $(LIBHANDLE) $(LIBFROG) $(LIBUNISTRING)
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

default: depend $(LTCOMMAND)

phase5.o unicrash.o xfs.o: $(TOPDIR)/include/builddefs

include $(BUILDRULES)

install: default $(INSTALL_SCRUB)

install-scrub:
	$(INSTALL) -m 755 -d $(PKG_ROOT_SBIN_DIR)
	$(LTINSTALL) -m 755 $(LTCOMMAND) $(PKG_ROOT_SBIN_DIR)

install-dev:

-include .dep
