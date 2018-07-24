// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016 Red Hat, Inc.
 * All Rights Reserved.
 */

struct field;

extern void	crc_init(void);
extern void	crc_struct(const field_t *fields, int argc, char **argv);
