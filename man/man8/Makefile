#
# Copyright (c) 2000-2001 Silicon Graphics, Inc.  All Rights Reserved.
#

TOPDIR = ../..
include $(TOPDIR)/include/builddefs

MAN_SECTION	= 8

MAN_PAGES	= $(shell echo *.$(MAN_SECTION)) mkfs.xfs.8
MAN_DEST	= $(PKG_MAN_DIR)/man$(MAN_SECTION)
LSRCFILES	= $(MAN_PAGES)

default : $(MAN_PAGES)

LDIRT		= mkfs.xfs.8

include $(BUILDRULES)

mkfs.xfs.8: mkfs.xfs.8.in
	@echo "    [SED]    $@"
	$(Q)$(SED) -e "s|@sysconfdir@|$(PKG_ETC_DIR)|g" < $< > $@

install : default
	$(INSTALL) -m 755 -d $(MAN_DEST)
	$(INSTALL_MAN)
install-dev :
