/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "libxfs.h"
#include "stage.h"
#include "progress.h"
#include "bulk_extent.h"

struct range { xfs_fsblock_t start; xfs_extlen_t length;
    xfs_ino_t owner; xfs_fileoff_t offset; xfs_exntst_t state; };
static struct range *ranges;
static size_t count, capacity;

static void need(int error, const char *what)
{
    if (error) { errno = error < 0 ? -error : error; fatal(what); }
}

bool bulk_data_eligible(struct xfs_inode *ip)
{
    return S_ISREG(VFS_I(ip)->i_mode) && !xfs_is_reflink_inode(ip) &&
        ip->i_df.if_format == XFS_DINODE_FMT_EXTENTS;
}

/* The attribute fork has already been released. Shared files and btree
 * forks use normal libxfs truncation. Preserve individual reverse-map
 * removals (or rebuild their index); free-space work is combined. */
void bulk_data_collect(struct xfs_trans *tp, struct xfs_inode *ip, size_t limit)
{
    struct xfs_iext_cursor cursor;
    struct xfs_bmbt_irec extent;
    uint64_t blocks = 0;
    unsigned seen = 0;
    if (xfs_need_iread_extents(&ip->i_df))
        need(libxfs_iread_extents(tp, ip, XFS_DATA_FORK), "read bulk data extents");
    for_each_xfs_iext(&ip->i_df, &cursor, &extent) {
        if (!libxfs_verify_fsbext(ip->i_mount, extent.br_startblock, extent.br_blockcount)) {
            errno = EFSCORRUPTED; fatal("invalid bulk data extent");
        }
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 1024;
            if (next > limit) next = limit;
            if (next <= capacity) { errno = ENOMEM; fatal("extent-index memory limit reached; source unchanged"); }
            ranges = realloc(ranges, next * sizeof *ranges);
            if (!ranges) fatal("allocate extent index");
            capacity = next;
        }
        ranges[count++] = (struct range){extent.br_startblock, extent.br_blockcount, ip->i_ino, extent.br_startoff, extent.br_state};
        blocks += extent.br_blockcount;
        ++seen;
    }
    if (blocks != ip->i_nblocks || seen != ip->i_df.if_nextents) {
        errno = EFSCORRUPTED; fatal("bulk data block count mismatch");
    }
    ip->i_nblocks = 0;
    ip->i_df.if_nextents = 0;
}

static int compare(const void *a, const void *b)
{
    const struct range *x = a, *y = b;
    return (x->start > y->start) - (x->start < y->start);
}

struct rebuild {
    size_t first, end, matched, nr, capacity, nblocks, oldblocks, used, loaded;
    struct xfs_rmap_rec *records;
    xfs_agblock_t *blocks;
};

static int save_rmap(struct xfs_btree_cur *cur, const union xfs_btree_rec *disk, void *priv)
{
    struct rebuild *b = priv;
    struct xfs_rmap_irec map;
    size_t lo = b->first, hi = b->end;
    xfs_fsblock_t start;
    if (xfs_rmap_btrec_to_irec(disk, &map) || xfs_rmap_check_irec(to_perag(cur->bc_group), &map))
        return -EFSCORRUPTED;
    start = XFS_AGB_TO_FSB(cur->bc_mp, pag_agno(to_perag(cur->bc_group)), map.rm_startblock);
    while (lo < hi) {
        size_t middle = lo + (hi-lo)/2;
        if (ranges[middle].start < start) lo = middle + 1;
        else hi = middle;
    }
    if (lo < b->end) {
        struct range *r = &ranges[lo];
        if (r->start == start && r->length == map.rm_blockcount && r->owner == map.rm_owner &&
            r->offset == map.rm_offset &&
            map.rm_flags == (r->state == XFS_EXT_UNWRITTEN ? XFS_RMAP_UNWRITTEN : 0)) {
            ++b->matched;
            return 0;
        }
    }
    if (b->nr >= b->capacity) return -EFSCORRUPTED;
    b->records[b->nr++] = disk->rmap;
    return 0;
}

static int save_rmap_block(struct xfs_btree_cur *cur, int level, void *priv)
{
    struct rebuild *b = priv;
    struct xfs_buf *bp;
    libxfs_btree_get_block(cur, level, &bp);
    if (!bp || b->nblocks >= b->oldblocks) return -EFSCORRUPTED;
    b->blocks[b->nblocks++] = xfs_daddr_to_agbno(cur->bc_mp, xfs_buf_daddr(bp));
    return 0;
}

static int load_rmaps(struct xfs_btree_cur *cur, unsigned idx,
                      struct xfs_btree_block *block, unsigned wanted, void *priv)
{
    struct rebuild *b = priv;
    unsigned j;
    if (wanted > b->nr - b->loaded) return -EFSCORRUPTED;
    for (j = 0; j < wanted; ++j)
        libxfs_btree_rec_addr(cur, (int)(idx+j), block)->rmap = b->records[b->loaded++];
    return (int)wanted;
}

static int reuse_rmap_block(struct xfs_btree_cur *cur, union xfs_btree_ptr *ptr, void *priv)
{
    struct rebuild *b = priv;
    if (b->used >= b->nblocks) return -ENOSPC;
    ptr->s = cpu_to_be32(b->blocks[b->used++]);
    return 0;
}

static int compare_blocks(const void *a, const void *b)
{
    xfs_agblock_t x = *(const xfs_agblock_t *)a, y = *(const xfs_agblock_t *)b;
    return (x > y) - (x < y);
}

/* Rebuilding is useful only when deleting a substantial part of an AG's
 * reverse mappings. Bulk loading reuses the existing rmap blocks. Surplus
 * blocks return through the normal rmap-btree/AGFL accounting path. All of
 * this happens in the sector overlay, before source application. */
static bool rebuild_rmaps(struct xfs_mount *mp, struct xfs_perag *pag, size_t first, size_t end)
{
    struct rebuild b = {.first = first, .end = end};
    struct xfs_buf *agbp;
    struct xfs_agf *agf;
    struct xfs_btree_cur *cur;
    struct xfs_trans *tp;
    struct xbtree_afakeroot fake = {0};
    struct xfs_btree_bload loader = {.get_records = load_rmaps,
        .claim_block = reuse_rmap_block, .leaf_slack = 0, .node_slack = 0};
    uint64_t upper;
    size_t j;
    bool rebuilt = false;
    if (end - first < 1024) return false;
    need(libxfs_alloc_read_agf(pag, NULL, 0, &agbp), "read reverse-map geometry");
    agf = agbp->b_addr;
    b.oldblocks = be32_to_cpu(agf->agf_rmap_blocks);
    upper = (uint64_t)b.oldblocks * mp->m_rmap_mxr[0];
    if (!b.oldblocks || upper > (end-first)*4U || upper > SIZE_MAX / sizeof *b.records) {
        libxfs_buf_relse(agbp);
        return false;
    }
    b.capacity = (size_t)upper;
    b.records = malloc(b.capacity * sizeof *b.records);
    b.blocks = malloc(b.oldblocks * sizeof *b.blocks);
    if (!b.records || !b.blocks) fatal("allocate reverse-map rebuild index");
    cur = libxfs_rmapbt_init_cursor(mp, NULL, agbp, pag);
    need(xfs_btree_query_all(cur, save_rmap, &b), "filter reverse mappings");
    if (b.matched != end-first) {
        libxfs_btree_del_cursor(cur, XFS_BTREE_NOERROR);
        libxfs_buf_relse(agbp);
        goto out;
    }
    need(libxfs_btree_visit_blocks(cur, save_rmap_block, XFS_BTREE_VISIT_ALL, &b), "collect reverse-map blocks");
    libxfs_btree_del_cursor(cur, XFS_BTREE_NOERROR);
    libxfs_buf_relse(agbp);
    if (b.nblocks != b.oldblocks) { errno = EFSCORRUPTED; fatal("reverse-map block count mismatch"); }
    qsort(b.blocks, b.nblocks, sizeof *b.blocks, compare_blocks);
    for (j = 1; j < b.nblocks; ++j)
        if (b.blocks[j] == b.blocks[j-1]) { errno = EFSCORRUPTED; fatal("duplicate reverse-map tree block"); }
    cur = libxfs_rmapbt_init_cursor(mp, NULL, NULL, pag);
    libxfs_btree_stage_afakeroot(cur, &fake);
    need(libxfs_btree_bload_compute_geometry(cur, &loader, b.nr), "size rebuilt reverse-map tree");
    if (loader.nr_blocks > b.nblocks) {
        libxfs_btree_del_cursor(cur, XFS_BTREE_NOERROR);
        goto out;
    }
    need(libxfs_btree_bload(cur, &loader, &b), "bulk-load surviving reverse mappings");
    need(libxfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, 0, 0, 0, &tp), "allocate rebuilt root transaction");
    need(libxfs_alloc_read_agf(pag, tp, 0, &agbp), "read rebuilt reverse-map header");
    xfs_rmapbt_commit_staged_btree(cur, tp, agbp);
    pag->pagf_rmap_level = fake.af_levels;
    /* Account surplus tree blocks until each is returned below. */
    agf = agbp->b_addr;
    agf->agf_rmap_blocks = cpu_to_be32((uint32_t)b.oldblocks);
    libxfs_btree_del_cursor(cur, XFS_BTREE_NOERROR);
    need(libxfs_trans_commit(tp), "commit rebuilt reverse-map root");
    for (j = b.used; j < b.nblocks; ++j) {
        struct xfs_buf *bp;
        need(libxfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, 0, 0, 0, &tp), "allocate surplus rmap block transaction");
        need(xfs_free_extent_fix_freelist(tp, pag, &agbp), "refresh surplus rmap freelist");
        if (!agbp) { errno = EFSCORRUPTED; fatal("missing surplus rmap header"); }
        cur = libxfs_rmapbt_init_cursor(mp, tp, agbp, pag);
        /* These blocks were verified while walking the old tree. They
         * are no longer in the new tree, whose height can be smaller. */
        need(libxfs_trans_get_buf(tp, mp->m_ddev_targp,
            XFS_AGB_TO_DADDR(mp, pag_agno(pag), b.blocks[j]), XFS_FSB_TO_BB(mp, 1), 0,
            &bp), "get surplus rmap block");
        need(cur->bc_ops->free_block(cur, bp), "return surplus rmap block");
        libxfs_trans_binval(tp, bp);
        libxfs_btree_del_cursor(cur, XFS_BTREE_NOERROR);
        need(libxfs_trans_commit(tp), "commit surplus rmap block");
    }
    fprintf(stderr, "AG %u: bulk rebuilt %zu reverse mappings, kept %zu records\n", pag_agno(pag), b.matched, b.nr);
    rebuilt = true;
out:
    free(b.records); free(b.blocks);
    return rebuilt;
}

/* Sort owner updates by physical block and reuse a cursor for a larger
 * batch. No inode is held across these transactions. */
static void remove_reverse_maps(struct xfs_mount *mp)
{
    size_t i = 0;
    if (!xfs_has_rmapbt(mp)) return;
    while (i < count) {
        size_t ag_end = i + 1;
        xfs_agnumber_t ag = XFS_FSB_TO_AGNO(mp, ranges[i].start);
        struct xfs_perag *pag = libxfs_perag_get(mp, ag);
        if (!pag) { errno = EFSCORRUPTED; fatal("missing reverse-map allocation group"); }
        while (ag_end < count && XFS_FSB_TO_AGNO(mp, ranges[ag_end].start) == ag) ++ag_end;
        if (rebuild_rmaps(mp, pag, i, ag_end)) {
            i = ag_end;
            progress_set(i);
            libxfs_perag_put(pag);
            continue;
        }
        while (i < ag_end) {
            struct xfs_trans *tp;
            struct xfs_btree_cur *cursor = NULL;
            size_t end = i + 1024;
            if (end > ag_end) end = ag_end;
            need(libxfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, 0, 0, 0, &tp), "allocate reverse-map batch");
            for (; i < end; ++i) {
                struct range *r = &ranges[i];
                struct xfs_rmap_intent intent = {0};
                intent.ri_type = XFS_RMAP_UNMAP;
                intent.ri_whichfork = XFS_DATA_FORK;
                intent.ri_owner = r->owner;
                intent.ri_group = pag_group(pag);
                intent.ri_bmap = (struct xfs_bmbt_irec){
                    .br_startoff = r->offset, .br_startblock = r->start,
                    .br_blockcount = r->length, .br_state = r->state};
                need(xfs_rmap_finish_one(tp, &intent, &cursor), "remove data reverse mapping");
            }
            progress_set(i);
            if (cursor) libxfs_btree_del_cursor(cursor, XFS_BTREE_NOERROR);
            need(libxfs_trans_commit(tp), "commit reverse-map batch to staging");
        }
        libxfs_perag_put(pag);
    }
}

void bulk_data_free(struct xfs_mount *mp)
{
    size_t i, groups = 0;
    progress_phase("sort data extents", NULL, 0);
    if (count > 1) qsort(ranges, count, sizeof *ranges, compare);
    for (i = 1; i < count; ++i) {
        if (ranges[i].start < ranges[i-1].start + ranges[i-1].length) {
            errno = EFSCORRUPTED; fatal("overlapping unshared data extents");
        }
    }
    progress_phase("update reverse mappings", "extents", count);
    remove_reverse_maps(mp);
    progress_phase("prepare combined extent frees", "extents", count);
    for (i = 0; i < count;) {
        xfs_fsblock_t start = ranges[i].start;
        xfs_agnumber_t ag = XFS_FSB_TO_AGNO(mp, start);
        xfs_extlen_t length = ranges[i++].length;
        struct xfs_trans *tp;
        struct xfs_perag *pag;
        while (i < count && ranges[i].start == start + length &&
               XFS_FSB_TO_AGNO(mp, ranges[i].start) == ag &&
               ranges[i].length <= UINT32_MAX - length)
            length += ranges[i++].length;
        need(libxfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, 0, 0, 0, &tp), "allocate bulk extent transaction");
        pag = libxfs_perag_get(mp, ag);
        if (!pag) { errno = EFSCORRUPTED; fatal("missing data allocation group"); }
        need(__xfs_free_extent(tp, pag, XFS_FSB_TO_AGBNO(mp, start), length,
                              &XFS_RMAP_OINFO_SKIP_UPDATE, XFS_AG_RESV_NONE, true), "free combined data extent");
        need(libxfs_trans_commit(tp), "commit bulk extent to staging");
        libxfs_perag_put(pag);
        ++groups;
        progress_set(i);
    }
    fprintf(stderr, "Combined %zu data extents into %zu free-space updates\n", count, groups);
    free(ranges); ranges = NULL; count = capacity = 0;
}
