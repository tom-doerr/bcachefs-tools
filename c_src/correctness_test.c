// SPDX-License-Identifier: GPL-2.0
/* Storage reliability regressions. Call only with disposable regular-file images. */
#include <stdio.h>

#include "libbcachefs.h"
#include "fs/alloc/disk_groups.h"
#include "fs/btree/bkey_buf.h"
#include "fs/btree/key_cache.h"
#include "fs/btree/locking.h"
#include "fs/btree/update.h"
#include "fs/data/ec/create.h"
#include "fs/data/ec/trigger.h"
#include "fs/data/reconcile/trigger.h"
#include "fs/init/fs.h"
#include "fs/journal/init.h"
#include "fs/journal/journal.h"
#include "fs/journal/reclaim.h"
#include "fs/journal/sb.h"

int bch2_test_reconcile_pending(struct bch_fs *);
int rust_test_storage_correctness(const char **, unsigned);
int rust_test_storage_flush_error(const char **, unsigned);
extern void bch_test_fail_next_flush(int) __attribute__((weak));
extern unsigned bch_test_flush_failures(void) __attribute__((weak));

#define check(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%u: %s\n", __func__, __LINE__, #cond); \
		return -EINVAL; \
	} \
} while (0)

static int first_stripe(struct btree_trans *trans, struct bkey_buf *saved)
{
	struct bkey_s_c k;
	int ret;
	for_each_btree_key_norestart(trans, iter, BTREE_ID_stripes, POS_MIN, 0, k, ret) {
		if (k.k->type == KEY_TYPE_stripe) {
			bch2_bkey_buf_reassemble(saved, k);
			return 0;
		}
	}
	return ret ?: -ENOENT;
}

static int check_journal_array(struct bch_sb *sb, const u64 *expected, unsigned nr)
{
	struct bch_sb_field_journal_v2 *v = bch2_sb_field_get(sb, journal_v2);
	check(v);
	unsigned n = 0;
	for (unsigned i = 0; i < bch2_sb_field_journal_v2_nr_entries(v); i++)
		for (u64 j = 0; j < le64_to_cpu(v->d[i].nr); j++) {
			check(n < nr);
			check(le64_to_cpu(v->d[i].start) + j == expected[n++]);
		}
	check(n == nr);
	return 0;
}

/* Exercise the deletion caller and re-read its persisted superblock. */
static int journal_bucket_delete(struct bch_fs *c, const char **paths)
{
	struct bch_dev *ca = bch2_dev_have_ref(c, 0);
	struct journal_device *ja = &ca->journal;
	unsigned nr = ja->nr;
	check(nr >= 3);
	u64 *original __free(kfree) = kmemdup(ja->buckets, nr * sizeof(u64), GFP_KERNEL);
	u64 *expected __free(kfree) = kmemdup(ja->buckets, nr * sizeof(u64), GFP_KERNEL);
	u64 *seqs __free(kfree) = kmemdup(ja->bucket_seq, nr * sizeof(u64), GFP_KERNEL);
	check(original && expected && seqs);
	unsigned discard = ja->discard_idx, dirty = ja->dirty_idx;
	unsigned ondisk = ja->dirty_idx_ondisk, cur = ja->cur_idx;
	unsigned pos = nr / 2;
	memmove(expected + pos, expected + pos + 1, (nr - pos - 1) * sizeof(u64));
	try(bch2_dev_journal_bucket_delete(ca, original[pos]));
	int ret = check_journal_array(ca->disk_sb.sb, expected, nr - 1);
	if (!ret && (ja->nr != nr - 1 || memcmp(ja->buckets, expected, (nr - 1) * sizeof(u64))))
		ret = -EINVAL;
	struct bch_sb_handle disk = {};
	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, read_only, true);
	if (!ret)
		ret = bch2_read_super(paths[0], &opts, &disk);
	if (!ret)
		ret = check_journal_array(disk.sb, expected, nr - 1);
	bch2_free_super(&disk);

	/* Restore the fixture's bucket accounting before opening it writable. */
	scoped_guard(mutex_noio, &c->sb_lock) {
		memcpy(ja->buckets, original, nr * sizeof(u64));
		memcpy(ja->bucket_seq, seqs, nr * sizeof(u64));
		ja->nr = nr;
		ja->discard_idx = discard;
		ja->dirty_idx = dirty;
		ja->dirty_idx_ondisk = ondisk;
		ja->cur_idx = cur;
		int restore = bch2_journal_buckets_to_sb(c, ca, original, nr) ?: bch2_write_super(c);
		ret = ret ?: restore;
	}
	fprintf(stderr, "storage reliability: persisted middle journal-bucket deletion: %d\n", ret);
	return ret;
}

static int key_cache_pin_relock(struct bch_fs *c, struct bpos pos, bool fixed)
{
	struct journal *j = &c->journal;
	/* Keep background reclaim out of this deterministic two-transaction interleaving. */
	guard(mutex)(&j->reclaim_lock);
	CLASS(btree_trans, observer)(c);
	CLASS(btree_iter, iter)(observer, BTREE_ID_stripes, pos, BTREE_ITER_cached|BTREE_ITER_intent);
	struct bkey_s_c k = bkey_try(bch2_btree_iter_peek_slot(&iter));
	check(k.k->type == KEY_TYPE_stripe);
	struct bkey_cached *ck = (void *) btree_iter_path(observer, &iter)->l[0].b;
	check(!test_bit(BKEY_CACHED_DIRTY, &ck->flags));
	check(!ck->journal.seq);
	u64 saved_seq = ck->seq;
	CLASS(closure_stack, cl)();
	struct journal_res res = {};
	try(bch2_journal_res_get(j, &res, jset_u64s(0), BCH_WATERMARK_reclaim, NULL));
	u64 old_seq = res.seq;
	set_bit(BKEY_CACHED_DIRTY, &ck->flags);
	atomic_long_inc(&c->btree.key_cache.nr_dirty);
	bch2_journal_pin_set(j, old_seq, &ck->journal, bch2_btree_key_cache_journal_flush);
	bch2_journal_res_flush(j, &res, &cl);
	bch2_journal_res_put(j, &res);
	bch2_trans_unlock(observer);
	closure_sync(&cl);
	int ret = bch2_journal_error(j);
	if (!ret) {
		res = (struct journal_res) {};
		ret = bch2_journal_res_get(j, &res, jset_u64s(0), BCH_WATERMARK_reclaim, NULL);
	}
	if (!ret) {
		ck->seq = res.seq;
		bch2_journal_res_put(j, &res);
		ret = ck->seq > old_seq ? 0 : -EINVAL;
	}
	if (!ret)
		ret = bch2_btree_iter_traverse(&iter);
	if (!ret) {
		bch2_trans_unlock(observer);
		if (fixed) {
			ret = bch2_btree_key_cache_journal_flush(j, &ck->journal, old_seq);
		} else {
			CLASS(btree_trans, reclaim)(c);
			btree_path_idx_t path;
			ret = bch2_btree_node_lock_with_path(reclaim, &ck->c, SIX_LOCK_intent, 0, &path);
			if (!ret) {
				bch2_journal_pin_update(j, ck->seq, &ck->journal,
						bch2_btree_key_cache_journal_flush);
				bch2_btree_node_unlock_with_path(reclaim, path, 0);
			}
		}
		if (!ret) {
			int relock = bch2_trans_relock_notrace(observer);
			fprintf(stderr, "storage reliability: key-cache %s relock=%d\n",
				fixed ? "production callback" : "old intent-only control", relock);
			ret = (fixed ? bch2_err_matches(relock, BCH_ERR_transaction_restart) : !relock)
				&& ck->journal.seq == ck->seq ? 0 : -EINVAL;
		}
	}
	bch2_trans_unlock(observer);
	bch2_trans_begin(observer);
	bch2_journal_pin_drop(j, &ck->journal);
	clear_bit(BKEY_CACHED_DIRTY, &ck->flags);
	atomic_long_dec(&c->btree.key_cache.nr_dirty);
	ck->seq = saved_seq;
	return ret;
}

static int stripe_reconcile_marker(struct btree_trans *trans, const struct bkey_buf *stripe)
{
	struct bkey_buf copy __cleanup(bch2_bkey_buf_exit);
	bch2_bkey_buf_init(&copy);
	bch2_bkey_buf_reassemble(&copy, bkey_i_to_s_c(stripe->k));
	struct bkey_i_stripe *s = bkey_i_to_stripe(copy.k);
	s->v.needs_reconcile = true;
	struct bch_inode_opts opts;
	bch2_inode_opts_get(trans->c, &opts, false);
	try(bch2_bkey_set_needs_reconcile(trans, NULL, &opts, bkey_i_to_s(copy.k),
					copy.k->k.u64s, SET_NEEDS_RECONCILE_foreground, 0));
	check(!s->v.needs_reconcile);
	fprintf(stderr, "storage reliability: recovered stripe clears reconciliation marker\n");
	return 0;
}

int rust_test_storage_correctness(const char **paths, unsigned nr)
{
	darray_const_str devs = { .data = paths, .nr = nr };
	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, read_only, true);
	opt_set(opts, copygc_enabled, false);
	opt_set(opts, journal_rewind_discard_buffer_percent, 0);
	opt_set(opts, reconcile_enabled, false);
	opt_set(opts, auto_snapshot_deletion, false);
	struct bch_fs *c = bch2_fs_open(&devs, &opts, NULL);
	if (IS_ERR(c))
		return PTR_ERR(c);
	int ret = journal_bucket_delete(c, paths);
	struct bkey_buf stripe __cleanup(bch2_bkey_buf_exit);
	bch2_bkey_buf_init(&stripe);
	if (!ret) {
		CLASS(btree_trans, trans)(c);
		ret = lockrestart_do(trans, first_stripe(trans, &stripe)) ?:
			lockrestart_do(trans, stripe_reconcile_marker(trans, &stripe));
	}
	if (!ret) {
		c->opts.read_only = false;
		ret = bch2_fs_read_write(c);
	}
	for (unsigned fixed = 0; !ret && fixed < 2; fixed++)
		ret = key_cache_pin_relock(c, stripe.k->k.p, fixed);
	if (!ret)
		ret = bch2_test_reconcile_pending(c);
	int exit_ret = bch2_fs_exit(c);
	return ret ?: exit_ret;
}

int rust_test_storage_flush_error(const char **paths, unsigned nr)
{
	check(bch_test_fail_next_flush && bch_test_flush_failures);
	check(nr == 3);
	darray_const_str devs = { .data = paths, .nr = nr };
	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, copygc_enabled, false);
	opt_set(opts, reconcile_enabled, false);
	opt_set(opts, auto_snapshot_deletion, false);
	/* Journal goes to member 0; member 1 still needs its data flushed. */
	opt_set(opts, metadata_target, dev_to_target(0));
	opt_set(opts, journal_rewind_discard_buffer_percent, 0);
	struct bch_fs *c = bch2_fs_open(&devs, &opts, NULL);
	if (IS_ERR(c))
		return PTR_ERR(c);
	struct bch_dev *ca = bch2_dev_have_ref(c, 1);
	int ret = bch2_journal_meta(&c->journal);
	if (!ret) {
		u64 previous = ca->prev_journal_sector;
		bch_test_fail_next_flush(ca->disk_sb.bdev->bd_fd);
		int flush_ret = bch2_journal_meta(&c->journal);
		unsigned failures = bch_test_flush_failures();
		bch_test_fail_next_flush(-1);
		fprintf(stderr, "storage reliability: nonjournal member flush failures=%u ret=%d journal_error=%d\n",
			failures, flush_ret, bch2_journal_error(&c->journal));
		ret = failures == 1 && flush_ret && bch2_journal_error(&c->journal) &&
			ca->prev_journal_sector == previous ? 0 : -EINVAL;
	}
	int exit_ret = bch2_fs_exit(c);
	if (exit_ret && !bch2_err_matches(exit_ret, BCH_ERR_shutdown_with_emergency_ro))
		ret = ret ?: exit_ret;
	return ret;
}
