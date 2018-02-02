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
xfs_scrub.h

CFILES = \
common.c \
xfs_scrub.c

LLDLIBS += $(LIBHANDLE) $(LIBFROG) $(LIBPTHREAD)
LTDEPENDENCIES += $(LIBHANDLE) $(LIBFROG)
LLDFLAGS = -static

default: depend $(LTCOMMAND)

include $(BUILDRULES)

install: default $(INSTALL_SCRUB)

install-scrub:
	$(INSTALL) -m 755 -d $(PKG_ROOT_SBIN_DIR)
	$(LTINSTALL) -m 755 $(LTCOMMAND) $(PKG_ROOT_SBIN_DIR)

install-dev:

-include .dep
