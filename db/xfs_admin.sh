#!/bin/sh -f
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2000-2001 Silicon Graphics, Inc.  All Rights Reserved.
#

status=0
DB_OPTS=""
REPAIR_OPTS=""
REPAIR_DEV_OPTS=""
IO_OPTS=""
LOG_OPTS=""
USAGE="Usage: xfs_admin [-efjlpuV] [-c 0|1] [-L label] [-O v5_feature] [-r rtdev] [-U uuid] device [logdev]"

# Try to find a loop device associated with a file.  We only want to return
# one loopdev (multiple loop devices can attach to a single file) so we grab
# the last line and return it if it's actually a block device.
try_find_loop_dev_for_file() {
	local x="$(losetup -O NAME -j "$1" 2> /dev/null | tail -n 1)"
	test -b "$x" && echo "$x"
}

try_find_mount_point_for_bdev() {
	local arg="$1"

	# See if we can map the arg to a loop device
	loopdev="$(try_find_loop_dev_for_file "${arg}")"
	test -n "${loopdev}" && arg="${loopdev}"

	if [ ! -b "${arg}" ]; then
		return 1
	fi

	# If we find a mountpoint for the device, do a live query;
	# otherwise try reading the fs with xfs_db.
	findmnt -t xfs -f -n -o TARGET "${arg}" 2> /dev/null
}

while getopts "c:efjlL:O:pr:uU:V" c
do
	case $c in
	c)	REPAIR_OPTS=$REPAIR_OPTS" -c lazycount="$OPTARG;;
	e)	DB_OPTS=$DB_OPTS" -c 'version extflg'";;
	f)	DB_OPTS=$DB_OPTS" -f";;
	j)	DB_OPTS=$DB_OPTS" -c 'version log2'";;
	l)	DB_OPTS=$DB_OPTS" -r -c label";
		IO_OPTS=$IO_OPTS" -r -c label";;
	L)	DB_OPTS=$DB_OPTS" -c 'label "$OPTARG"'";
		IO_OPTS=$IO_OPTS" -c 'label -s "$OPTARG"'";;
	O)	REPAIR_OPTS=$REPAIR_OPTS" -c $OPTARG";;
	p)	DB_OPTS=$DB_OPTS" -c 'version projid32bit'";;
	r)	REPAIR_DEV_OPTS=" -r '$OPTARG'";;
	u)	DB_OPTS=$DB_OPTS" -r -c uuid";;
	U)	DB_OPTS=$DB_OPTS" -c 'uuid "$OPTARG"'";;
	V)	xfs_db -p xfs_admin -V
		status=$?
		exit $status
		;;
	\?)	echo $USAGE 1>&2
		exit 2
		;;
	esac
done
set -- extra $@
shift $OPTIND
case $# in
	1|2)
		# Pick up the log device, if present
		if [ -n "$2" ]; then
			LOG_OPTS=" -l '$2'"
		fi

		if [ -n "$IO_OPTS" ]; then
			mntpt="$(try_find_mount_point_for_bdev "$1")"
			if [ $? -eq 0 ]; then
				eval xfs_io -x -p xfs_admin $IO_OPTS "$mntpt"
				status=$?
				DB_OPTS=""
				REPAIR_OPTS=""
			fi
		fi

		if [ -n "$DB_OPTS" ]
		then
			eval xfs_db -x -p xfs_admin $LOG_OPTS $DB_OPTS "$1"
			status=$?
		fi
		if [ -n "$REPAIR_OPTS" ]
		then
			echo "Running xfs_repair to upgrade filesystem."
			eval xfs_repair $LOG_OPTS $REPAIR_DEV_OPTS $REPAIR_OPTS "$1"
			status=`expr $? + $status`
		fi
		;;
	*)	echo $USAGE 1>&2
		exit 2
		;;
esac
exit $status
