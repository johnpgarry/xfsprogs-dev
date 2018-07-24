// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "quota.h"

int
xfsquotactl(
	int		command,
	const char	*device,
	uint		type,
	uint		id,
	void		*addr)
{
	errno = -ENOSYS;
	return -1;
}
