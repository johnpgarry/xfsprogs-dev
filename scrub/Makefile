#
# Copyright (C) 2018 Oracle.  All Rights Reserved.
#

TOPDIR = ..
include $(TOPDIR)/include/builddefs

# On linux we get fsmap from the system or define it ourselves
# so include this based on platform type.  If this reverts to only
# the autoconf check w/o local definition, change to testing HAVE_GETFSMAP
SCRUB_PREREQS=$(PKG_PLATFORM)

ifeq ($(SCRUB_PREREQS),linux)
LTCOMMAND = xfs_scrub
INSTALL_SCRUB = install-scrub
endif	# scrub_prereqs

HFILES = \
common.h \
counter.h \
disk.h \
filemap.h \
fscounters.h \
inodes.h \
scrub.h \
spacemap.h \
unicrash.h \
xfs_scrub.h

CFILES = \
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
scrub.c \
spacemap.c \
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

default: depend $(LTCOMMAND)

phase5.o unicrash.o xfs.o: $(TOPDIR)/include/builddefs

include $(BUILDRULES)

install: default $(INSTALL_SCRUB)

install-scrub:
	$(INSTALL) -m 755 -d $(PKG_ROOT_SBIN_DIR)
	$(LTINSTALL) -m 755 $(LTCOMMAND) $(PKG_ROOT_SBIN_DIR)

install-dev:

-include .dep
