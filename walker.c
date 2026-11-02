// SPDX-License-Identifier: GPL-2.0-or-later
/* Directory walker adapted from xfsprogs v6.16.0 db/namei.c.
 * Copyright (C) 2021 Oracle. All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "libxfs.h"
#include "walker.h"
#include "config.h"
static int
list_sfdir(
	struct xfs_da_args		*args,
	dir_emit_t			dir_emit,
	void				*private)
{
	struct xfs_inode		*dp = args->dp;
	struct xfs_mount		*mp = dp->i_mount;
	struct xfs_da_geometry		*geo = args->geo;
	struct xfs_dir2_sf_entry	*sfep;
	struct xfs_dir2_sf_hdr		*sfp = dp->i_df.if_data;
	xfs_ino_t			ino;
	xfs_dir2_dataptr_t		off;
	unsigned int			i;
	uint8_t				filetype;
	int				error;

	/* . and .. entries */
	off = xfs_dir2_db_off_to_dataptr(geo, geo->datablk,
			geo->data_entry_offset);
	error = dir_emit(args->trans, args->dp, off, ".", -1, dp->i_ino,
			XFS_DIR3_FT_DIR, private);
	if (error)
		return error;

	ino = libxfs_dir2_sf_get_parent_ino(sfp);
	off = xfs_dir2_db_off_to_dataptr(geo, geo->datablk,
			geo->data_entry_offset +
			libxfs_dir2_data_entsize(mp, sizeof(".") - 1));
	error = dir_emit(args->trans, args->dp, off, "..", -1, ino,
			XFS_DIR3_FT_DIR, private);
	if (error)
		return error;

	/* Walk everything else. */
	sfep = xfs_dir2_sf_firstentry(sfp);
	for (i = 0; i < sfp->count; i++) {
		ino = libxfs_dir2_sf_get_ino(mp, sfp, sfep);
		filetype = libxfs_dir2_sf_get_ftype(mp, sfep);
		off = xfs_dir2_db_off_to_dataptr(geo, geo->datablk,
				xfs_dir2_sf_get_offset(sfep));

		error = dir_emit(args->trans, args->dp, off,
				(char *)sfep->name, sfep->namelen, ino,
				filetype, private);
		if (error)
			return error;
		sfep = libxfs_dir2_sf_nextentry(mp, sfp, sfep);
	}

	return 0;
}

/* List entries in block format directory. */
static int
list_blockdir(
	struct xfs_da_args	*args,
	dir_emit_t		dir_emit,
	void			*private)
{
	struct xfs_inode	*dp = args->dp;
	struct xfs_mount	*mp = dp->i_mount;
	struct xfs_buf		*bp;
	struct xfs_da_geometry	*geo = mp->m_dir_geo;
	xfs_dir2_dataptr_t	diroff;
	unsigned int		offset;
	unsigned int		end;
	int			error;

	error = xfs_dir3_block_read(args->trans, dp, args->owner, &bp);
	if (error)
		return error;

	end = xfs_dir3_data_end_offset(geo, bp->b_addr);
	for (offset = geo->data_entry_offset; offset < end;) {
		struct xfs_dir2_data_unused *dup = (void *)((char *)bp->b_addr + offset);
		struct xfs_dir2_data_entry *dep = (void *)((char *)bp->b_addr + offset);
		uint8_t				filetype;

		if (be16_to_cpu(dup->freetag) == XFS_DIR2_DATA_FREE_TAG) {
			/* Unused entry */
			offset += be16_to_cpu(dup->length);
			continue;
		}

		/* Real entry */
		diroff = xfs_dir2_db_off_to_dataptr(geo, geo->datablk, offset);
		offset += libxfs_dir2_data_entsize(mp, dep->namelen);
		filetype = libxfs_dir2_data_get_ftype(dp->i_mount, dep);
		error = dir_emit(args->trans, args->dp, diroff,
				(char *)dep->name, dep->namelen,
				be64_to_cpu(dep->inumber), filetype, private);
		if (error)
			break;
	}

	libxfs_trans_brelse(args->trans, bp);
	return error;
}

/* List entries in leaf format directory. */
static int
list_leafdir(
	struct xfs_da_args	*args,
	dir_emit_t		dir_emit,
	void			*private)
{
	struct xfs_bmbt_irec	map;
	struct xfs_iext_cursor	icur;
	struct xfs_inode	*dp = args->dp;
	struct xfs_mount	*mp = dp->i_mount;
	struct xfs_buf		*bp = NULL;
	struct xfs_ifork	*ifp = xfs_ifork_ptr(dp, XFS_DATA_FORK);
	struct xfs_da_geometry	*geo = mp->m_dir_geo;
	xfs_dir2_off_t		dirboff;
	xfs_dablk_t		dabno = 0;
	int			error = 0;

	/* Refuse an exceptional fork before libxfs loads its entire extent map. */
	if (ifp->if_nextents > DELTREE_FORK_EXTENTS) return E2BIG;
	/* Read extent map. */
	error = -libxfs_iread_extents(args->trans, dp, XFS_DATA_FORK);
	if (error)
		return error;

	while (dabno < geo->leafblk) {
		unsigned int	offset;
		unsigned int	length;

		/* Find mapping for leaf block. */
		if (!xfs_iext_lookup_extent(dp, ifp, dabno, &icur, &map))
			break;
		if (map.br_startoff >= geo->leafblk)
			break;
		libxfs_trim_extent(&map, dabno, geo->leafblk - dabno);

		/* Read the directory block of that first mapping. */
		error = xfs_dir3_data_read(args->trans, dp, args->owner,
				map.br_startoff, 0, &bp);
		if (error)
			break;

		dirboff = xfs_dir2_da_to_byte(geo, map.br_startoff);
		for (offset = geo->data_entry_offset; offset < geo->blksize;) {
			struct xfs_dir2_data_entry	*dep;
			struct xfs_dir2_data_unused	*dup;
			uint8_t				filetype;

			dup = (void *)((char *)bp->b_addr + offset);
			dep = (void *)((char *)bp->b_addr + offset);

			if (be16_to_cpu(dup->freetag) ==
			    XFS_DIR2_DATA_FREE_TAG) {
				/* Skip unused entry */
				length = be16_to_cpu(dup->length);
				offset += length;
				continue;
			}

			offset += libxfs_dir2_data_entsize(mp, dep->namelen);
			filetype = libxfs_dir2_data_get_ftype(mp, dep);

			error = dir_emit(args->trans, args->dp,
					xfs_dir2_byte_to_dataptr(dirboff + offset),
					(char *)dep->name, dep->namelen,
					be64_to_cpu(dep->inumber), filetype,
					private);
			if (error)
				break;
		}

		dabno = map.br_startoff + geo->fsbcount;
		libxfs_trans_brelse(args->trans, bp);
		bp = NULL;
		if (error) break;
	}

	if (bp)
		libxfs_trans_brelse(args->trans, bp);

	return error;
}

/* Read the directory, display contents. */
int
listdir(
	struct xfs_trans	*tp,
	struct xfs_inode	*dp,
	dir_emit_t		dir_emit,
	void			*private)
{
	struct xfs_da_args	args = {
		.trans		= tp,
		.dp		= dp,
		.geo		= dp->i_mount->m_dir_geo,
		.owner		= dp->i_ino,
	};
	int			error;

	switch (libxfs_dir2_format(&args, &error)) {
	case XFS_DIR2_FMT_SF:
		return list_sfdir(&args, dir_emit, private);
	case XFS_DIR2_FMT_BLOCK:
		return list_blockdir(&args, dir_emit, private);
	case XFS_DIR2_FMT_LEAF:
	case XFS_DIR2_FMT_NODE:
		return list_leafdir(&args, dir_emit, private);
	default:
		return EFSCORRUPTED;
	}
}
