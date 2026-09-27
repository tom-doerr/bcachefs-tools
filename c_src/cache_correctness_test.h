/* SPDX-License-Identifier: GPL-2.0 */
/* Included by btree/cache.c only in userspace; uses disposable image fixtures. */
#include "btree/key_cache.h"
#include "btree/update.h"
#include "btree/write_buffer.h"

int bch2_test_cache_cannibalize(struct bch_fs *);
DEFINE_DARRAY_NAMED(darray_test_cache_nodes, struct btree *);

static int test_cache_dirty_leaf(struct btree_trans *trans, struct btree **target,
				struct bkey_buf *saved)
{
	CLASS(btree_iter, iter)(trans, BTREE_ID_alloc, POS_MIN, BTREE_ITER_intent);
	iter.flags &= ~BTREE_ITER_with_key_cache;
	struct bkey_s_c k;
	int ret;
	for_each_btree_key_continue_norestart(iter, 0, k, ret) {
		struct btree *b = btree_iter_path(trans, &iter)->l[0].b;
		if (btree_node_permanent(b) || btree_node_noevict(b) ||
		    btree_node_write_blocked(b) || btree_node_never_write(b) ||
		    btree_node_write_in_flight(b) || !b->written)
			continue;

		/* Force a real leaf commit, retaining identical allocation contents. */
		bch2_bkey_buf_reassemble(saved, k);
		errptr_try(bch2_bkey_make_mut(trans, &iter, &k,
					    BTREE_UPDATE_key_cache_reclaim|BTREE_TRIGGER_norun));
		try(bch2_trans_commit(trans, NULL, NULL,
			BCH_TRANS_COMMIT_no_enospc|BCH_TRANS_COMMIT_no_skip_noops|BCH_WATERMARK_reclaim));
		b = btree_iter_path(trans, &iter)->l[0].b;
		if (!btree_node_dirty(b) || btree_node_permanent(b))
			return -EINVAL;
		set_btree_node_noevict(b);
		*target = b;
		return 0;
	}
	return ret ?: -ENOENT;
}

static int test_cache_reread(struct btree_trans *trans, const struct bkey_buf *saved)
{
	CLASS(btree_iter, iter)(trans, BTREE_ID_alloc, saved->k->k.p, 0);
	iter.flags &= ~BTREE_ITER_with_key_cache;
	struct bkey_s_c k = bkey_try(bch2_btree_iter_peek_slot(&iter));
	return k.k->type == saved->k->k.type &&
		bkey_val_bytes(k.k) == bkey_val_bytes(&saved->k->k) &&
		!memcmp(k.v, &saved->k->v, bkey_val_bytes(k.k)) ? 0 : -EINVAL;
}

int bch2_test_cache_cannibalize(struct bch_fs *c)
{
	CLASS(btree_trans, trans)(c);
	try(bch2_btree_write_buffer_flush_sync(trans));
	bch2_trans_unlock(trans);
	/* Positive means that the flush made progress, not an error. */
	int ret = bch2_btree_key_cache_flush_going_ro(c);
	if (ret < 0)
		return ret;
	bch2_btree_flush_all_writes(c);

	/* Keep journal reclaim from writing the selected leaf before the test. */
	guard(mutex)(&c->journal.reclaim_lock);
	struct btree *target = NULL;
	struct bkey_buf saved __cleanup(bch2_bkey_buf_exit);
	bch2_bkey_buf_init(&saved);
	try(lockrestart_do(trans, test_cache_dirty_leaf(trans, &target, &saved)));
	unsigned written = target->written;
	bch2_trans_unlock(trans);

	struct bch_fs_btree_cache *bc = &c->btree.cache;
	CLASS(darray_test_cache_nodes, held)();
	ret = bch2_btree_cache_cannibalize_lock(trans, NULL);
	if (ret)
		goto out;

	/* Select this leaf without exhausting memory or disturbing other caches. */
	scoped_guard(mutex_noio, &bc->lock) {
		for (unsigned i = 0; !ret && i < ARRAY_SIZE(bc->live); i++) {
			struct list_head *lists[] = { &bc->live[i].clean, &bc->live[i].dirty };
			for (unsigned j = 0; !ret && j < ARRAY_SIZE(lists); j++) {
				struct btree *b;
				list_for_each_entry(b, lists[j], list) {
					if (b == target || btree_node_noevict(b))
						continue;
					ret = darray_push(&held, b);
					if (ret)
						break;
					set_btree_node_noevict(b);
				}
			}
		}
		clear_btree_node_noevict(target);
	}
	if (!ret) {
		struct btree *reclaimed = btree_node_cannibalize(trans, target->c.lock.readers != NULL);
		if (!reclaimed) {
			ret = -EIO;
		} else {
			ret = reclaimed == target && reclaimed->written > written &&
				!btree_node_dirty(reclaimed) && !btree_node_write_in_flight(reclaimed) &&
				!btree_node_hashed(reclaimed) ? 0 : -EINVAL;
			fprintf(stderr, "storage reliability: dirty cache reclaim wrote %u -> %u sectors, ret=%d\n",
				written, reclaimed->written, ret);
			bch2_btree_node_transition_state(bc, reclaimed, BTREE_NODE_CACHE_FREEABLE);
			six_unlock_write(&reclaimed->c.lock);
			six_unlock_intent(&reclaimed->c.lock);
		}
	}
	/* Every temporary noevict bit belongs to this test. */
	darray_for_each(held, b)
		clear_btree_node_noevict(*b);
	bch2_btree_cache_cannibalize_unlock(trans);
out:
	clear_btree_node_noevict(target);
	if (!ret)
		ret = lockrestart_do(trans, test_cache_reread(trans, &saved));
	return ret;
}
