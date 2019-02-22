#!/bin/sh -f
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2000-2001 Silicon Graphics, Inc.  All Rights Reserved.
#

OPTS=""
USAGE="Usage: xfs_info [-V] [-t mtab] [mountpoint|device|file]"

while getopts "t:V" c
do
	case $c in
	t)	OPTS="-t $OPTARG" ;;
	V)	xfs_spaceman -p xfs_info -V
		status=$?
		exit $status
		;;
	*)	echo $USAGE 1>&2
		exit 2
		;;
	esac
done
set -- extra "$@"
shift $OPTIND
case $# in
	1)
		arg="$1"

		# See if we can map the arg to a loop device
		loopdev="$(losetup -n -O NAME -j "${arg}" 2> /dev/null)"
		test -n "${loopdev}" && arg="${loopdev}"

		# If we find a mountpoint for the device, do a live query;
		# otherwise try reading the fs with xfs_db.
		if mountpt="$(findmnt -f -n -o TARGET "${arg}" 2> /dev/null)"; then
			xfs_spaceman -p xfs_info -c "info" $OPTS "${mountpt}"
			status=$?
		else
			xfs_db -p xfs_info -c "info" $OPTS "${arg}"
			status=$?
		fi
		;;
	*)	echo $USAGE 1>&2
		exit 2
		;;
esac
exit $status
