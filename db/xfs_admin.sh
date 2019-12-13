#!/bin/sh -f
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2000-2001 Silicon Graphics, Inc.  All Rights Reserved.
#

status=0
DB_OPTS=""
REPAIR_OPTS=""
IO_OPTS=""
USAGE="Usage: xfs_admin [-efjlpuV] [-c 0|1] [-L label] [-U uuid] device [logdev]"

# Try to find a loop device associated with a file.  We only want to return
# one loopdev (multiple loop devices can attach to a single file) so we grab
# the last line and return it if it's actually a block device.
try_find_loop_dev_for_file() {
	local x="$(losetup -O NAME -j "$1" 2> /dev/null | tail -n 1)"
	test -b "$x" && echo "$x"
}

# See if we can find a mount point for the argument.
find_mntpt_for_arg() {
	local arg="$1"

	# See if we can map the arg to a loop device
	local loopdev="$(try_find_loop_dev_for_file "${arg}")"
	test -n "$loopdev" && arg="$loopdev"

	# If we find a mountpoint for the device, do a live query;
	# otherwise try reading the fs with xfs_db.
	findmnt -t xfs -f -n -o TARGET "${arg}" 2> /dev/null
}

while getopts "efjlpuc:L:U:V" c
do
	case $c in
	c)	REPAIR_OPTS=$REPAIR_OPTS" -c lazycount="$OPTARG;;
	e)	DB_OPTS=$DB_OPTS" -c 'version extflg'";;
	f)	DB_OPTS=$DB_OPTS" -f";;
	j)	DB_OPTS=$DB_OPTS" -c 'version log2'";;
	l)	DB_OPTS=$DB_OPTS" -r -c label"
		IO_OPTS=$IO_OPTS" -r -c label"
		;;
	L)	DB_OPTS=$DB_OPTS" -c 'label "$OPTARG"'"
		if [ "$OPTARG" = "--" ]; then
			IO_OPTS=$IO_OPTS" -c 'label -c'"
		else
			IO_OPTS=$IO_OPTS" -c 'label -s "$OPTARG"'"
		fi
		;;
	p)	DB_OPTS=$DB_OPTS" -c 'version projid32bit'";;
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
			DB_OPTS=$DB_OPTS" -l '$2'"
			test -n "$REPAIR_OPTS" && \
				REPAIR_OPTS=$REPAIR_OPTS" -l '$2'"
		fi

		# Try making the changes online, if supported
		if [ -n "$IO_OPTS" ] && mntpt="$(find_mntpt_for_arg "$1")"
		then
			eval xfs_io -x -p xfs_admin $IO_OPTS "$mntpt"
			test "$?" -eq 0 && exit 0
		fi

		# Otherwise try offline changing
		if [ -n "$DB_OPTS" ]
		then
			eval xfs_db -x -p xfs_admin $DB_OPTS $1
			status=$?
		fi
		if [ -n "$REPAIR_OPTS" ]
		then
			# Hide normal repair output which is sent to stderr
			# assuming the filesystem is fine when a user is
			# running xfs_admin.
			# Ideally, we need to improve the output behaviour
			# of repair for this purpose (say a "quiet" mode).
			eval xfs_repair $REPAIR_OPTS $1 2> /dev/null
			status=`expr $? + $status`
			if [ $status -ne 0 ]
			then
				echo "Conversion failed, is the filesystem unmounted?"
			fi
		fi
		;;
	*)	echo $USAGE 1>&2
		exit 2
		;;
esac
exit $status
