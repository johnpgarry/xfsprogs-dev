// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2011 RedHat, Inc.
 * All Rights Reserved.
 */
#ifndef __TRACE_H__
#define __TRACE_H__

#define trace_xfs_agfl_reset(a,b,c,d)		((void) 0)
#define trace_xfs_agfl_free_defer(a,b,c,d,e)	((void) 0)
#define trace_xfs_alloc_cur_check(a,b,c,d,e,f)	((void) 0)
#define trace_xfs_alloc_cur(a)			((void) 0)
#define trace_xfs_alloc_cur_left(a)		((void) 0)
#define trace_xfs_alloc_cur_lookup(a)		((void) 0)
#define trace_xfs_alloc_cur_lookup_done(a)	((void) 0)
#define trace_xfs_alloc_cur_right(a)		((void) 0)
#define trace_xfs_alloc_exact_done(a)		((void) 0)
#define trace_xfs_alloc_exact_notfound(a)	((void) 0)
#define trace_xfs_alloc_exact_error(a)		((void) 0)
#define trace_xfs_alloc_near_first(a)		((void) 0)
#define trace_xfs_alloc_near_greater(a)		((void) 0)
#define trace_xfs_alloc_near_lesser(a)		((void) 0)
#define trace_xfs_alloc_near_error(a)		((void) 0)
#define trace_xfs_alloc_near_noentry(a)		((void) 0)
#define trace_xfs_alloc_near_busy(a)		((void) 0)
#define trace_xfs_alloc_size_neither(a)		((void) 0)
#define trace_xfs_alloc_size_noentry(a)		((void) 0)
#define trace_xfs_alloc_size_nominleft(a)	((void) 0)
#define trace_xfs_alloc_size_done(a)		((void) 0)
#define trace_xfs_alloc_size_error(a)		((void) 0)
#define trace_xfs_alloc_size_busy(a)		((void) 0)
#define trace_xfs_alloc_small_freelist(a)	((void) 0)
#define trace_xfs_alloc_small_notenough(a)	((void) 0)
#define trace_xfs_alloc_small_done(a)		((void) 0)
#define trace_xfs_alloc_small_error(a)		((void) 0)
#define trace_xfs_alloc_vextent_badargs(a)	((void) 0)
#define trace_xfs_alloc_vextent_nofix(a)	((void) 0)
#define trace_xfs_alloc_vextent_noagbp(a)	((void) 0)
#define trace_xfs_alloc_vextent_loopfailed(a)	((void) 0)
#define trace_xfs_alloc_vextent_allfailed(a)	((void) 0)
#define trace_xfs_alloc_vextent_skip_deadlock(...)	((void)0)

#define trace_xfs_attr_defer_add(...)		((void) 0)
#define trace_xfs_attr_defer_replace(...)	((void) 0)
#define trace_xfs_attr_defer_remove(...)	((void) 0)
#define trace_xfs_attr_sf_addname_return(...)	((void) 0)
#define trace_xfs_attr_set_iter_return(...)	((void) 0)
#define trace_xfs_attr_leaf_addname_return(...)	((void) 0)
#define trace_xfs_attr_node_addname_return(...)	((void) 0)
#define trace_xfs_attr_remove_iter_return(...)	((void) 0)
#define trace_xfs_attr_rmtval_alloc(...)	((void) 0)
#define trace_xfs_attr_rmtval_remove_return(...) ((void) 0)

#define trace_xfs_log_recover_item_add_cont(a,b,c,d)	((void) 0)
#define trace_xfs_log_recover_item_add(a,b,c,d)	((void) 0)

#define trace_xfs_da_btree_corrupt(a,b)		((void) 0)
#define trace_xfs_btree_corrupt(a,b)		((void) 0)
#define trace_xfs_btree_updkeys(a,b,c)		((void) 0)
#define trace_xfs_btree_overlapped_query_range(a,b,c)	((void) 0)
#define trace_xfs_btree_commit_afakeroot(a)	((void) 0)
#define trace_xfs_btree_commit_ifakeroot(a)	((void) 0)
#define trace_xfs_btree_bload_level_geometry(a,b,c,d,e,f,g) ((void) 0)
#define trace_xfs_btree_bload_block(a,b,c,d,e,f) ((void) 0)

#define trace_xfs_free_extent(a,b,c,d,e,f,g)	((void) 0)
#define trace_xfs_agf(a,b,c,d)			((void) 0)
#define trace_xfs_read_agf(a,b)			((void) 0)
#define trace_xfs_alloc_read_agf(a,b)		((void) 0)
#define trace_xfs_read_agi(a,b)			((void) 0)
#define trace_xfs_ialloc_read_agi(a,b)		((void) 0)
#define trace_xfs_irec_merge_pre(a,b,c,d,e,f)	((void) 0)
#define trace_xfs_irec_merge_post(a,b,c,d)	((void) 0)

#define trace_xfs_iext_insert(a,b,c,d)		((void) 0)
#define trace_xfs_iext_remove(a,b,c,d)		((void) 0)

#define trace_xfs_defer_relog_intent(a,b)	((void) 0)

#define trace_xfs_dir2_grow_inode(a,b)		((void) 0)
#define trace_xfs_dir2_shrink_inode(a,b)	((void) 0)

#define trace_xfs_dir2_leaf_to_node(a)	((void) 0)
#define trace_xfs_dir2_leaf_to_block(a)	((void) 0)
#define trace_xfs_dir2_leaf_addname(a)	((void) 0)
#define trace_xfs_dir2_leaf_lookup(a)	((void) 0)
#define trace_xfs_dir2_leaf_removename(a)	((void) 0)
#define trace_xfs_dir2_leaf_replace(a)	((void) 0)

#define trace_xfs_dir2_block_addname(a)	((void) 0)
#define trace_xfs_dir2_block_to_leaf(a)	((void) 0)
#define trace_xfs_dir2_block_to_sf(a)	((void) 0)
#define trace_xfs_dir2_block_lookup(a)	((void) 0)
#define trace_xfs_dir2_block_removename(a)	((void) 0)
#define trace_xfs_dir2_block_replace(a)	((void) 0)

#define trace_xfs_dir2_leafn_add(a,b)	((void) 0)
#define trace_xfs_dir2_leafn_remove(a,b)	((void) 0)
#define trace_xfs_dir2_leafn_moveents(a,b,c,d)	((void) 0)

#define trace_xfs_dir2_node_to_leaf(a)	((void) 0)
#define trace_xfs_dir2_node_addname(a)	((void) 0)
#define trace_xfs_dir2_node_lookup(a)	((void) 0)
#define trace_xfs_dir2_node_removename(a)	((void) 0)
#define trace_xfs_dir2_node_replace(a)	((void) 0)

#define trace_xfs_dir2_sf_to_block(a)	((void) 0)
#define trace_xfs_dir2_sf_addname(a)	((void) 0)
#define trace_xfs_dir2_sf_create(a)	((void) 0)
#define trace_xfs_dir2_sf_lookup(a)	((void) 0)
#define trace_xfs_dir2_sf_removename(a)	((void) 0)
#define trace_xfs_dir2_sf_replace(a)	((void) 0)
#define trace_xfs_dir2_sf_toino4(a)	((void) 0)
#define trace_xfs_dir2_sf_toino8(a)	((void) 0)

#define trace_xfs_da_node_create(a)		((void) 0)
#define trace_xfs_da_split(a)			((void) 0)
#define trace_xfs_attr_leaf_split_before(a)	((void) 0)
#define trace_xfs_attr_leaf_split_after(a)	((void) 0)
#define trace_xfs_da_root_split(a)		((void) 0)
#define trace_xfs_da_node_split(a)		((void) 0)
#define trace_xfs_da_node_rebalance(a)		((void) 0)
#define trace_xfs_da_node_add(a)		((void) 0)
#define trace_xfs_da_join(a)			((void) 0)
#define trace_xfs_da_root_join(a)		((void) 0)
#define trace_xfs_da_node_toosmall(a)		((void) 0)
#define trace_xfs_da_fixhashpath(a)		((void) 0)
#define trace_xfs_da_node_remove(a)		((void) 0)
#define trace_xfs_da_node_unbalance(a)		((void) 0)
#define trace_xfs_da_link_before(a)		((void) 0)
#define trace_xfs_da_link_after(a)		((void) 0)
#define trace_xfs_da_unlink_back(a)		((void) 0)
#define trace_xfs_da_unlink_forward(a)		((void) 0)
#define trace_xfs_da_path_shift(a)		((void) 0)
#define trace_xfs_da_grow_inode(a)		((void) 0)
#define trace_xfs_da_swap_lastblock(a)		((void) 0)
#define trace_xfs_da_shrink_inode(a)		((void) 0)

#define trace_xfs_attr_sf_create(a)		((void) 0)
#define trace_xfs_attr_sf_add(a)		((void) 0)
#define trace_xfs_attr_sf_remove(a)		((void) 0)
#define trace_xfs_attr_sf_lookup(a)		((void) 0)
#define trace_xfs_attr_sf_to_leaf(a)		((void) 0)
#define trace_xfs_attr_leaf_to_sf(a)		((void) 0)
#define trace_xfs_attr_leaf_to_node(a)		((void) 0)
#define trace_xfs_attr_leaf_create(a)		((void) 0)
#define trace_xfs_attr_leaf_split(a)		((void) 0)
#define trace_xfs_attr_leaf_add_old(a)		((void) 0)
#define trace_xfs_attr_leaf_add_new(a)		((void) 0)
#define trace_xfs_attr_leaf_add(a)		((void) 0)
#define trace_xfs_attr_leaf_add_work(a)		((void) 0)
#define trace_xfs_attr_leaf_compact(a)		((void) 0)
#define trace_xfs_attr_leaf_rebalance(a)	((void) 0)
#define trace_xfs_attr_leaf_toosmall(a)		((void) 0)
#define trace_xfs_attr_leaf_remove(a)		((void) 0)
#define trace_xfs_attr_leaf_unbalance(a)	((void) 0)
#define trace_xfs_attr_leaf_lookup(a)		((void) 0)
#define trace_xfs_attr_leaf_clearflag(a)	((void) 0)
#define trace_xfs_attr_leaf_setflag(a)		((void) 0)
#define trace_xfs_attr_leaf_flipflags(a)	((void) 0)

#define trace_xfs_attr_sf_addname(a)		((void) 0)
#define trace_xfs_attr_leaf_addname(a)		((void) 0)
#define trace_xfs_attr_leaf_replace(a)		((void) 0)
#define trace_xfs_attr_leaf_removename(a)	((void) 0)
#define trace_xfs_attr_leaf_get(a)		((void) 0)
#define trace_xfs_attr_node_addname(a)		((void) 0)
#define trace_xfs_attr_node_replace(a)		((void) 0)
#define trace_xfs_attr_node_removename(a)	((void) 0)
#define trace_xfs_attr_fillstate(a)		((void) 0)
#define trace_xfs_attr_refillstate(a)		((void) 0)
#define trace_xfs_attr_node_get(a)		((void) 0)
#define trace_xfs_attr_rmtval_get(a)		((void) 0)
#define trace_xfs_attr_rmtval_set(a)		((void) 0)
#define trace_xfs_attr_rmtval_remove(a)		((void) 0)

#define trace_xfs_bmap_pre_update(a,b,c,d)	((void) 0)
#define trace_xfs_bmap_post_update(a,b,c,d)	((void) 0)
#define trace_xfs_bunmap(a,b,c,d,e)		((void) 0)
#define trace_xfs_read_extent(a,b,c,d)		((void) 0)

/* set c = c to avoid unused var warnings */
#define trace_xfs_write_extent(a,b,c,d)	((c) = (c))
#define trace_xfs_perag_get(a,b,c,d)	((c) = (c))
#define trace_xfs_perag_get_tag(a,b,c,d) ((c) = (c))
#define trace_xfs_perag_put(a,b,c,d)	((c) = (c))

#define trace_xfs_trans_alloc(a,b)		((void) 0)
#define trace_xfs_trans_cancel(a,b)		((void) 0)
#define trace_xfs_trans_brelse(a)		((void) 0)
#define trace_xfs_trans_binval(a)		((void) 0)
#define trace_xfs_trans_bjoin(a)		((void) 0)
#define trace_xfs_trans_bhold(a)		((void) 0)
#define trace_xfs_trans_bhold_release(a)	((void) 0)
#define trace_xfs_trans_get_buf(a)		((void) 0)
#define trace_xfs_trans_get_buf_recur(a)	((void) 0)
#define trace_xfs_trans_log_buf(a)		((void) 0)
#define trace_xfs_trans_getsb_recur(a)		((void) 0)
#define trace_xfs_trans_getsb(a)		((void) 0)
#define trace_xfs_trans_read_buf_recur(a)	((void) 0)
#define trace_xfs_trans_read_buf(a)		((void) 0)
#define trace_xfs_trans_commit(a,b)		((void) 0)
#define trace_xfs_trans_resv_calc_minlogsize(a,b,c) ((void) 0)
#define trace_xfs_log_get_max_trans_res(a,b)	((void) 0)

#define trace_xfs_defer_cancel(a,b)		((void) 0)
#define trace_xfs_defer_pending_commit(a,b)	((void) 0)
#define trace_xfs_defer_pending_abort(a,b)	((void) 0)
#define trace_xfs_defer_pending_finish(a,b)	((void) 0)
#define trace_xfs_defer_trans_abort(a,b)	((void) 0)
#define trace_xfs_defer_trans_roll(a,b)		((void) 0)
#define trace_xfs_defer_trans_roll_error(a,b)	((void) 0)
#define trace_xfs_defer_finish(a,b)		((void) 0)
#define trace_xfs_defer_finish_error(a,b)	((void) 0)
#define trace_xfs_defer_finish_done(a,b)	((void) 0)
#define trace_xfs_defer_cancel_list(a,b)	((void) 0)
#define trace_xfs_defer_create_intent(a,b)	((void) 0)

#define trace_xfs_bmap_free_defer(...)		((void) 0)
#define trace_xfs_bmap_free_deferred(...)	((void) 0)

#define trace_xfs_rmap_map(...)			((void) 0)
#define trace_xfs_rmap_map_error(...)		((void) 0)
#define trace_xfs_rmap_map_done(...)		((void) 0)
#define trace_xfs_rmap_unmap(...)		((void) 0)
#define trace_xfs_rmap_unmap_error(...)		((void) 0)
#define trace_xfs_rmap_unmap_done(...)		((void) 0)
#define trace_xfs_rmap_insert(...)		((void) 0)
#define trace_xfs_rmap_insert_error(...)	((void) 0)
#define trace_xfs_rmap_delete(...)		((void) 0)
#define trace_xfs_rmap_convert(...)		((void) 0)
#define trace_xfs_rmap_convert_state(...)	((void) 0)
#define trace_xfs_rmap_convert_done(...)	((void) 0)
#define trace_xfs_rmap_convert_error(...)	((void) 0)
#define trace_xfs_rmap_update(...)		((void) 0)
#define trace_xfs_rmap_update_error(...)	((void) 0)
#define trace_xfs_rmap_defer(...)		((void) 0)
#define trace_xfs_rmap_deferred(...)		((void) 0)
#define trace_xfs_rmap_find_right_neighbor_result(...)	((void) 0)
#define trace_xfs_rmap_find_left_neighbor_result(...)	((void) 0)
#define trace_xfs_rmap_lookup_le_range_result(...)	((void) 0)

#define trace_xfs_rmapbt_free_block(...)	((void) 0)
#define trace_xfs_rmapbt_alloc_block(...)	((void) 0)

#define trace_xfs_ag_resv_critical(...)		((void) 0)
#define trace_xfs_ag_resv_needed(...)		((void) 0)
#define trace_xfs_ag_resv_free(...)		((void) 0)
#define trace_xfs_ag_resv_free_error(...)	((void) 0)
#define trace_xfs_ag_resv_init(...)		((void) 0)
#define trace_xfs_ag_resv_init_error(...)	((void) 0)
#define trace_xfs_ag_resv_alloc_extent(...)	((void) 0)
#define trace_xfs_ag_resv_free_extent(...)	((void) 0)

#define trace_xfs_refcount_lookup(...)		((void) 0)
#define trace_xfs_refcount_get(...)		((void) 0)
#define trace_xfs_refcount_update(...)		((void) 0)
#define trace_xfs_refcount_update_error(...)	((void) 0)
#define trace_xfs_refcount_insert(...)		((void) 0)
#define trace_xfs_refcount_insert_error(...)	((void) 0)
#define trace_xfs_refcount_delete(...)		((void) 0)
#define trace_xfs_refcount_delete_error(...)	((void) 0)
#define trace_xfs_refcountbt_free_block(...)	((void) 0)
#define trace_xfs_refcountbt_alloc_block(...)	((void) 0)
#define trace_xfs_refcount_rec_order_error(...)	((void) 0)

#define trace_xfs_refcount_lookup(...)		((void) 0)
#define trace_xfs_refcount_get(...)		((void) 0)
#define trace_xfs_refcount_update(...)		((void) 0)
#define trace_xfs_refcount_update_error(...)	((void) 0)
#define trace_xfs_refcount_insert(...)		((void) 0)
#define trace_xfs_refcount_insert_error(...)	((void) 0)
#define trace_xfs_refcount_delete(...)		((void) 0)
#define trace_xfs_refcount_delete_error(...)	((void) 0)
#define trace_xfs_refcountbt_free_block(...)	((void) 0)
#define trace_xfs_refcountbt_alloc_block(...)	((void) 0)
#define trace_xfs_refcount_rec_order_error(...)	((void) 0)
#define trace_xfs_refcount_split_extent(...)	((void) 0)
#define trace_xfs_refcount_split_extent_error(...)		((void) 0)
#define trace_xfs_refcount_merge_center_extents_error(...)	((void) 0)
#define trace_xfs_refcount_merge_left_extent_error(...)		((void) 0)
#define trace_xfs_refcount_merge_right_extent_error(...)	((void) 0)
#define trace_xfs_refcount_find_left_extent(...)	((void) 0)
#define trace_xfs_refcount_find_left_extent_error(...)	((void) 0)
#define trace_xfs_refcount_find_right_extent(...)	((void) 0)
#define trace_xfs_refcount_find_right_extent_error(...)	((void) 0)
#define trace_xfs_refcount_merge_center_extents(...)	((void) 0)
#define trace_xfs_refcount_merge_left_extent(...)	((void) 0)
#define trace_xfs_refcount_merge_right_extent(...)	((void) 0)
#define trace_xfs_refcount_modify_extent(...)		((void) 0)
#define trace_xfs_refcount_modify_extent_error(...)	((void) 0)
#define trace_xfs_refcount_adjust_error(...)		((void) 0)
#define trace_xfs_refcount_increase(...)		((void) 0)
#define trace_xfs_refcount_decrease(...)		((void) 0)
#define trace_xfs_refcount_deferred(...)		((void) 0)
#define trace_xfs_refcount_defer(...)			((void) 0)
#define trace_xfs_refcount_finish_one_leftover(...)	((void) 0)
#define trace_xfs_refcount_find_shared(...)		((void) 0)
#define trace_xfs_refcount_find_shared_result(...)	((void) 0)
#define trace_xfs_refcount_find_shared_error(...)	((void) 0)

#define trace_xfs_bmap_remap_alloc(...)		((void) 0)
#define trace_xfs_bmap_deferred(...)		((void) 0)
#define trace_xfs_bmap_defer(...)		((void) 0)

#define trace_xfs_refcount_adjust_cow_error(...)	((void) 0)
#define trace_xfs_refcount_cow_increase(...)	((void) 0)
#define trace_xfs_refcount_cow_decrease(...)	((void) 0)
#define trace_xfs_refcount_recover_extent(...)	((void) 0)

#define trace_xfs_rmap_find_left_neighbor_candidate(...)	((void) 0)
#define trace_xfs_rmap_find_left_neighbor_query(...)	((void) 0)
#define trace_xfs_rmap_find_left_neighbor_result(...)	((void) 0)
#define trace_xfs_rmap_lookup_le_range_candidate(...)	((void) 0)
#define trace_xfs_rmap_lookup_le_range(...)	((void) 0)
#define trace_xfs_rmap_unmap(...)		((void) 0)
#define trace_xfs_rmap_unmap_done(...)		((void) 0)
#define trace_xfs_rmap_unmap_error(...)		((void) 0)
#define trace_xfs_rmap_map(...)			((void) 0)
#define trace_xfs_rmap_map_done(...)		((void) 0)
#define trace_xfs_rmap_map_error(...)		((void) 0)
#define trace_xfs_rmap_delete_error(...)	((void) 0)

#define trace_xfs_fs_mark_healthy(a,b)		((void) 0)

/* set c = c to avoid unused var warnings */
#define trace_xfs_perag_get(a,b,c,d)		((c) = (c))
#define trace_xfs_perag_get_tag(a,b,c,d)	((c) = (c))
#define trace_xfs_perag_put(a,b,c,d)		((c) = (c))

#endif /* __TRACE_H__ */
