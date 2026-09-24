// SPDX-License-Identifier: GPL-2.0

#include "bcachefs.h"

#include "alloc/accounting.h"
#include "alloc/background.h"
#include "alloc/backpointers.h"
#include "alloc/buckets.h"
#include "alloc/disk_groups.h"
#include "alloc/foreground.h"

#include "btree/interior.h"
#include "btree/update.h"
#include "btree/write_buffer.h"

#include "data/compress.h"
#include "data/copygc.h"
#include "data/ec/create.h"
#include "data/ec/trigger.h"
#include "data/move.h"
#include "data/reconcile/work.h"
#include "data/write.h"

#include "init/error.h"
#include "init/progress.h"

#include "fs/inode.h"
#include "fs/namei.h"

#include "sb/counters.h"
#include "snapshots/subvolume.h"

#include "util/clock.h"

#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/sched/cputime.h>

#define RECONCILE_PHASE_TYPES()		\
	x(scan)				\
	x(btree)			\
	x(phys)				\
	x(normal)			\
	x(destage)			\

enum reconcile_phase_type {
#define x(n)	RECONCILE_PHASE_##n,
	RECONCILE_PHASE_TYPES()
#undef x
};

#define x(n) #n,

const char * const bch2_reconcile_opts[] = {
	BCH_RECONCILE_OPTS()
	NULL
};

static const char * const bch2_reconcile_work_ids[] = {
	RECONCILE_WORK_IDS()
	NULL
};

static const char * const bch2_rebalance_scan_strs[] = {
	RECONCILE_SCAN_TYPES()
};

static const char * const bch2_reconcile_phase_types[] = {
	RECONCILE_PHASE_TYPES()
};

static const char * const bch2_reconcile_kick_reasons[] = {
	RECONCILE_KICK_REASONS()
	NULL
};

static const char * const bch2_reconcile_phase_exits[] = {
	RECONCILE_PHASE_EXITS()
	NULL
};

static const char * const bch2_move_outcomes[] = {
	MOVE_OUTCOMES()
	NULL
};

#undef x

static u64 reconcile_scan_encode(struct reconcile_scan s)
{
	switch (s.type) {
	case RECONCILE_SCAN_fs:
		return RECONCILE_SCAN_COOKIE_fs;
	case RECONCILE_SCAN_metadata:
		return RECONCILE_SCAN_COOKIE_metadata;
	case RECONCILE_SCAN_pending:
		return RECONCILE_SCAN_COOKIE_pending;
	case RECONCILE_SCAN_stripes:
		return RECONCILE_SCAN_COOKIE_stripes;
	case RECONCILE_SCAN_device:
		return RECONCILE_SCAN_COOKIE_device + s.dev;
	case RECONCILE_SCAN_inum:
		return s.inum;
	default:
		BUG();
	}
}

static struct reconcile_scan reconcile_scan_decode(struct bch_fs *c, u64 v)
{
	if (v >= BCACHEFS_ROOT_INO)
		return (struct reconcile_scan) { .type = RECONCILE_SCAN_inum, .inum = v, };
	if (v >= RECONCILE_SCAN_COOKIE_device)
		return (struct reconcile_scan) {
			.type = RECONCILE_SCAN_device,
			.dev =  v - RECONCILE_SCAN_COOKIE_device,
		};
	if (v == RECONCILE_SCAN_COOKIE_pending)
		return (struct reconcile_scan) { .type = RECONCILE_SCAN_pending };
	if (v == RECONCILE_SCAN_COOKIE_stripes)
		return (struct reconcile_scan) { .type = RECONCILE_SCAN_stripes };
	if (v == RECONCILE_SCAN_COOKIE_metadata)
		return (struct reconcile_scan) { .type = RECONCILE_SCAN_metadata };
	if (v == RECONCILE_SCAN_COOKIE_fs)
		return (struct reconcile_scan) { .type = RECONCILE_SCAN_fs};

	bch_err(c, "unknown reconcile scan cookie %llu", v);
	return (struct reconcile_scan) { .type = RECONCILE_SCAN_fs};
}

static __cold void reconcile_scan_to_text(struct printbuf *out,
				   struct bch_fs *c, struct reconcile_scan s)
{
	prt_str(out, bch2_rebalance_scan_strs[s.type]);
	switch (s.type) {
	case RECONCILE_SCAN_device:
		prt_str(out, ": ");
		bch2_prt_member_name(out, c, s.dev);
		break;
	case RECONCILE_SCAN_inum:
		prt_str(out, ": ");
		bch2_trans_do(c, bch2_inum_snapshot_to_path(trans, s.inum, 0, NULL, out));
		break;
	default:
		break;
	}
}

int bch2_set_reconcile_needs_scan_trans(struct btree_trans *trans, struct reconcile_scan s)
{
	CLASS(btree_iter, iter)(trans, BTREE_ID_reconcile_scan,
				POS(0, reconcile_scan_encode(s)),
				BTREE_ITER_intent);
	struct bkey_s_c k = bkey_try(bch2_btree_iter_peek_slot(&iter));

	u64 v = k.k->type == KEY_TYPE_cookie
		? le64_to_cpu(bkey_s_c_to_cookie(k).v->cookie)
		: 0;

	struct bkey_i_cookie *cookie = errptr_try(bch2_trans_kmalloc(trans, sizeof(*cookie)));

	bkey_cookie_init(&cookie->k_i);
	cookie->k.p = iter.pos;
	cookie->v.cookie = cpu_to_le64(v + 1);

	return bch2_trans_update(trans, &iter, &cookie->k_i, 0);
}

int bch2_set_reconcile_needs_scan(struct bch_fs *c, struct reconcile_scan s, bool wakeup)
{
	CLASS(btree_trans, trans)(c);
	try(commit_do(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc,
		      bch2_set_reconcile_needs_scan_trans(trans, s)));
	if (wakeup)
		bch2_reconcile_wakeup(c, RECONCILE_KICK_scan_cookie);
	return 0;
}

/*
 * In-flight opt changes:
 *
 * An opt change is a multi-step operation - it brackets the actual change with
 * scan-cookie bumps (see opts.c) so writers' extent triggers re-derive against
 * the new option as it lands. But the reconcile thread mustn't *complete*
 * (delete) a scan cookie for a pass that overlapped a half-applied opt change:
 * such a pass scanned against the intermediate option value, and on the
 * ERO/error path nothing bumps the cookie afterwards to force another pass. So
 * an opt change registers the cookie it touches here for its duration;
 * bch2_clear_reconcile_needs_scan() refuses to delete a registered cookie.
 *
 * Refcounted: more than one opt change can target the same cookie at once.
 *
 * Registration also lets the reconcile thread skip *starting* a scan it
 * couldn't complete anyway - but that's just sparing wasted work; the
 * don't-delete is what's load-bearing.
 */
struct reconcile_scan_in_flight {
	struct rhash_head	hash;
	u64			cookie;
	unsigned		ref;	/* protected by scans_in_flight_lock */
	struct rcu_head		rcu;
};

static const struct rhashtable_params reconcile_scan_in_flight_params = {
	.head_offset		= offsetof(struct reconcile_scan_in_flight, hash),
	.key_offset		= offsetof(struct reconcile_scan_in_flight, cookie),
	.key_len		= sizeof(u64),
	.automatic_shrinking	= true,
};

/* Lockless - called by the reconcile thread when deciding whether to clear a cookie: */
static bool reconcile_scan_in_flight(struct bch_fs *c, u64 cookie)
{
	return rhashtable_lookup_fast(&c->reconcile.scans_in_flight, &cookie,
				      reconcile_scan_in_flight_params) != NULL;
}

static int reconcile_scan_in_flight_get(struct bch_fs *c, u64 cookie)
{
	struct bch_fs_reconcile *r = &c->reconcile;

	guard(mutex)(&r->scans_in_flight_lock);

	struct reconcile_scan_in_flight *e =
		rhashtable_lookup_fast(&r->scans_in_flight, &cookie,
				       reconcile_scan_in_flight_params);
	if (e) {
		e->ref++;
		return 0;
	}

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return bch_err_throw(c, ENOMEM_reconcile_scan_in_flight);
	e->cookie	= cookie;
	e->ref		= 1;

	int ret = rhashtable_insert_fast(&r->scans_in_flight, &e->hash,
					 reconcile_scan_in_flight_params);
	if (ret)
		kfree(e);
	return ret;
}

static void reconcile_scan_in_flight_put(struct bch_fs *c, u64 cookie)
{
	struct bch_fs_reconcile *r = &c->reconcile;

	guard(mutex)(&r->scans_in_flight_lock);

	struct reconcile_scan_in_flight *e =
		rhashtable_lookup_fast(&r->scans_in_flight, &cookie,
				       reconcile_scan_in_flight_params);
	BUG_ON(!e);
	if (!--e->ref) {
		BUG_ON(rhashtable_remove_fast(&r->scans_in_flight, &e->hash,
					      reconcile_scan_in_flight_params));
		kfree_rcu(e, rcu);
	}
}

static void opt_change_scope_push(struct opt_change_scope *scope, u64 cookie)
{
	BUG_ON(scope->nr >= ARRAY_SIZE(scope->cookies));
	scope->cookies[scope->nr++] = cookie;
}

void bch2_opt_change_scope_exit(struct opt_change_scope *scope)
{
	while (scope->nr)
		reconcile_scan_in_flight_put(scope->c, scope->cookies[--scope->nr]);
}

/*
 * An opt change is about to start: register the cookie (recording it in the
 * caller's opt_change_scope, whose destructor unregisters it) so the reconcile
 * thread won't complete a pass against the intermediate state, then bump the
 * cookie so writers' extent triggers re-derive against the new option as it
 * lands. Should be paired with bch2_set_reconcile_needs_scan_post() once the
 * change has settled - but the registration is dropped by the scope destructor
 * regardless, so an erroring-out change doesn't strand it.
 */
int bch2_set_reconcile_needs_scan_pre(struct bch_fs *c, struct reconcile_scan s,
				      struct opt_change_scope *scope)
{
	u64 cookie = reconcile_scan_encode(s);

	try(reconcile_scan_in_flight_get(c, cookie));
	opt_change_scope_push(scope, cookie);

	CLASS(btree_trans, trans)(c);
	return commit_do(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc,
			 bch2_set_reconcile_needs_scan_trans(trans, s));
}

/*
 * The opt change has settled: bump the cookie again so it reflects the new
 * option, and kick the reconcile thread. (The in-flight registration is
 * released by the caller's opt_change_scope destructor, not here.)
 */
int bch2_set_reconcile_needs_scan_post(struct bch_fs *c, struct reconcile_scan s)
{
	CLASS(btree_trans, trans)(c);
	int ret = commit_do(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc,
			    bch2_set_reconcile_needs_scan_trans(trans, s));

	bch2_reconcile_wakeup(c, RECONCILE_KICK_opt_change_settled);
	return ret;
}

int bch2_set_fs_needs_reconcile(struct bch_fs *c)
{
	return bch2_set_reconcile_needs_scan(c,
				(struct reconcile_scan) { .type = RECONCILE_SCAN_fs },
				true);
}

int bch2_reconcile_scan_cookie_is_set(struct btree_trans *trans, u64 inum)
{
	CLASS(btree_iter, iter)(trans, BTREE_ID_reconcile_scan, POS(0, inum), 0);
	struct bkey_s_c k = bkey_try(bch2_btree_iter_peek_slot(&iter));
	return k.k->type == KEY_TYPE_cookie;
}

static int bch2_clear_reconcile_needs_scan(struct btree_trans *trans, struct bpos pos, u64 cookie)
{
	struct bch_fs *c = trans->c;
	u64 v;

	try(commit_do(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc, ({
		CLASS(btree_iter, iter)(trans, BTREE_ID_reconcile_scan, pos, BTREE_ITER_intent);
		struct bkey_s_c k = bch2_btree_iter_peek_slot(&iter);
		bkey_err(k) ?: ({
			v = k.k->type == KEY_TYPE_cookie
				? le64_to_cpu(bkey_s_c_to_cookie(k).v->cookie)
				: 0;
			v == cookie
				? bch2_btree_delete_at(trans, &iter, 0)
				: 0;
		});
	})));

	event_inc_trace(c, reconcile_clear_scan, buf, ({
		reconcile_scan_to_text(&buf, c, reconcile_scan_decode(c, pos.offset));
		prt_newline(&buf);
		prt_printf(&buf, "scan started with cookie %llu now have %llu", cookie, v);
		prt_printf(&buf, "%sdeleting scan cookie\n", v == cookie ? "" : "not ");
	}));
	return 0;
}

#define RECONCILE_WORK_BUF_NR		1024
DEFINE_DARRAY_NAMED(darray_reconcile_work, struct bkey_i);

static struct bkey_s_c next_reconcile_entry(struct btree_trans *trans,
					    darray_reconcile_work *buf,
					    struct bbpos *work_pos,
					    struct bpos end)
{
	enum btree_iter_update_trigger_flags flags = BTREE_ITER_prefetch;

	if (btree_type_has_snapshots(work_pos->btree))
		flags |= BTREE_ITER_all_snapshots;

	if (work_pos->btree == BTREE_ID_reconcile_scan) {
		buf->nr = 0;

		int ret = for_each_btree_key_max(trans, iter, work_pos->btree, work_pos->pos, end,
				   flags, k, ({
			bkey_reassemble(&darray_top(*buf), k);
			return bkey_i_to_s_c(&darray_top(*buf));
			0;
		}));

		return ret ? bkey_s_c_err(ret) : bkey_s_c_null;
	}

	if (unlikely(!buf->nr)) {
		/* Avoid contention with write buffer flush: buffer up work entries in a darray */

		BUG_ON(!buf->size);;

		int ret = for_each_btree_key_max(trans, iter, work_pos->btree, work_pos->pos, end,
				   flags, k, ({
			bch2_progress_update_iter(trans, &trans->c->reconcile.progress, &iter);

			/* There might be leftover scan cookies from rebalance, pre reconcile upgrade: */
			if (k.k->type != KEY_TYPE_set)
				continue;

			BUG_ON(bkey_bytes(k.k) > sizeof(buf->data[0]));

			/* we previously used darray_make_room */
			bkey_reassemble(&darray_top(*buf), k);
			buf->nr++;

			work_pos->pos = bpos_successor(iter.pos);
			if (buf->nr == buf->size)
				break;
			0;
		}));
		if (ret)
			return bkey_s_c_err(ret);

		if (!buf->nr)
			return bkey_s_c_null;

		unsigned l = 0, r = buf->nr - 1;
		while (l < r) {
			swap(buf->data[l], buf->data[r]);
			l++;
			--r;
		}
	}

	return bkey_i_to_s_c(&darray_pop(buf));
}

static int extent_ec_pending(struct btree_trans *trans, struct bkey_ptrs_c ptrs)
{
	struct bch_fs *c = trans->c;

	guard(rcu)();
	bkey_for_each_ptr(ptrs, ptr) {
		struct bch_dev *ca = bch2_dev_rcu_noerror(c, ptr->dev);
		if (!ca)
			continue;

		struct bpos bucket = PTR_BUCKET_POS(ca, ptr);
		if (bch2_bucket_has_new_stripe(c, bucket_to_u64(bucket)))
			return true;
	}
	return false;
}

int bch2_extent_reconcile_pending_mod(struct btree_trans *, struct btree_iter *,
				      unsigned, struct bkey_s_c, bool);

static int reconcile_set_data_opts(struct btree_trans *trans,
				   struct btree_iter *iter,
				   unsigned level,
				   struct bkey_s_c k,
				   struct bch_inode_opts *opts,
				   struct data_update_opts *data_opts)
{
	struct bch_fs *c = trans->c;
	const struct bch_extent_reconcile *r = bch2_bkey_reconcile_opts(c, k);
	if (!r || !r->need_rb) /* Write buffer race? */
		return 0;

	data_opts->type			= BCH_DATA_UPDATE_reconcile;
	data_opts->target		= r->background_target;

	/*
	 * Never wait on the allocator mid-write: a blocked data update holds a
	 * read-time snapshot of the extent while other movers rewrite it, and
	 * the stale write is then discarded at index update time
	 * (data_update_useless_write_fail). Better to fail with freelist_empty
	 * and retry from a fresh read.
	 */
	data_opts->write_flags |= BCH_WRITE_alloc_nowait;

	/*
	 * we can't add/drop replicas from btree nodes incrementally, we always
	 * need to be able to spill over to the whole fs
	 */
	if (!r->hipri && !bkey_is_btree_ptr(k.k))
		data_opts->write_flags |= BCH_WRITE_only_specified_devs;

	struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(k);
	const union bch_extent_entry *entry;
	struct extent_ptr_decoded p;
	struct bch_extent_reconcile rb = *r;

	/*
	 * Same as bch2_bkey_get_io_opts(), but for the extent's own reconcile
	 * entry rather than the opts overlaid from it: an unknown checksum or
	 * compression type is an incompat feature and shouldn't have mounted,
	 * so this is only reachable if our versioning was wrong - and getting
	 * it wrong would otherwise BUG() in bch2_data_checksum_type_rb() or
	 * read off the end of __bch2_compression_opt_to_type[].
	 *
	 * The opts have been through that same fallback already, so they're
	 * safe to take:
	 */
	if (rb.data_checksum >= BCH_CSUM_OPT_NR)
		rb.data_checksum = opts->data_checksum;
	if (!bch2_compression_opt_valid(rb.background_compression))
		rb.background_compression = opts->background_compression;

	unsigned csum_type = bch2_data_checksum_type_rb(c, rb);
	unsigned compression_type = bch2_compression_opt_to_type(rb.background_compression);

	if (r->need_rb & BIT(BCH_RECONCILE_data_replicas)) {
		struct bkey_durability durability;
		try(bch2_bkey_durability(trans, k, &durability));

		if (durability.total <= r->data_replicas) {
			unsigned ptr_bit = 1;
			guard(rcu)();

			bkey_for_each_ptr(ptrs, ptr) {
				if (bch2_dev_bad_or_evacuating(c, ptr->dev))
					data_opts->ptrs_kill |= ptr_bit;
				ptr_bit <<= 1;
			}
		} else {
			/*
			 * Over-replicated: propose drops on a working copy and
			 * recompute the whole-key durability after each, so we only
			 * ever propose drops the apply path (bch2_bkey_drop_extra_*
			 * durability()) will actually take - otherwise reconcile
			 * respins on work that can't complete. Drop offline devices
			 * first, holding total durability; then hold online durability
			 * at data_replicas. Whole-pointer drops use the cached flag (a
			 * reversible exclusion); stripe-pointer drops measure a copy.
			 */
			struct bkey_i *n = errptr_try(bch2_bkey_make_mut_noupdate(trans, k));
			struct bkey_durability cur = durability;

			for (unsigned phase = 0; phase < 2; phase++) {
				bool online_floor = phase == 1;

				/* phase 0 (hold total) only matters if some durability is offline */
				if (!online_floor && cur.total == cur.online)
					continue;

				/* Drop entire pointers? */
				unsigned ptr_bit = 1;
				bkey_for_each_ptr(bch2_bkey_ptrs(bkey_i_to_s(n)), ptr) {
					bool offline = ptr->dev == BCH_SB_MEMBER_INVALID ||
						       !test_bit(ptr->dev, c->devs_online.d);

					if (!ptr->cached && (online_floor || offline)) {
						bool force = bch2_dev_bad_or_evacuating(c, ptr->dev);

						ptr->cached = true;
						struct bkey_durability d;
						try(bch2_bkey_durability(trans, bkey_i_to_s_c(n), &d));

						unsigned have = online_floor ? d.online : d.total;
						unsigned was  = online_floor ? cur.online : cur.total;

						/*
						 * Drop only if we still hold data_replicas, and
						 * either the device is bad/evacuating (move its data
						 * off) or the pointer actually contributed durability
						 * (have < was). A durability=0 pointer that isn't
						 * evacuating is a cache replica - it leaves durability
						 * unchanged, so we keep it. An evacuating device also
						 * reads as durability=0, but we drop it once the
						 * required durability is held by other devices.
						 */
						if (have >= r->data_replicas &&
						    (force || have < was)) {
							data_opts->ptrs_kill |= ptr_bit;
							cur = d;
						} else {
							ptr->cached = false;
						}
					}
					ptr_bit <<= 1;
				}

				/* Stripe ec? Gather candidates first: dropping a stripe
				 * pointer rewrites the entry list. */
				unsigned ec_bits[BCH_BKEY_PTRS_MAX], nr_ec = 0;
				const union bch_extent_entry *ec_entry;
				struct extent_ptr_decoded ec_p = {};

				ptr_bit = 1;
				bkey_for_each_ptr_decode(&n->k, bch2_bkey_ptrs_c(bkey_i_to_s_c(n)), ec_p, ec_entry) {
					bool offline = ec_p.ptr.dev == BCH_SB_MEMBER_INVALID ||
						       !test_bit(ec_p.ptr.dev, c->devs_online.d);

					if (ec_p.has_ec && !ec_p.ptr.cached &&
					    (online_floor || offline))
						ec_bits[nr_ec++] = ptr_bit;
					ptr_bit <<= 1;
				}

				for (unsigned i = 0; i < nr_ec; i++) {
					struct bkey_i *m = errptr_try(bch2_bkey_make_mut_noupdate(trans, bkey_i_to_s_c(n)));
					bch2_bkey_drop_ec_mask(c, m, ec_bits[i]);

					struct bkey_durability d;
					try(bch2_bkey_durability(trans, bkey_i_to_s_c(m), &d));

					unsigned have = online_floor ? d.online : d.total;
					if (have >= r->data_replicas) {
						data_opts->ptrs_kill_ec |= ec_bits[i];
						bch2_bkey_drop_ec_mask(c, n, ec_bits[i]);
						cur = d;
					}
				}
			}
		}
	}

	if (r->need_rb & BIT(BCH_RECONCILE_erasure_code)) {
		if (r->erasure_code) {
			/*
			 * Can a stripe form right now? If not (e.g. not enough
			 * RW devs in target with matching bucket_size), queueing
			 * the data_update would just keep failing and re-queueing
			 * forever. If EC is the only thing to do, park the extent
			 * on the pending list — a device add/remove/state change
			 * will re-evaluate. Otherwise drop EC from the rb mask and
			 * fall through to do the other work.
			 */
			if (!bch2_can_form_ec_stripe(c, r->background_target, r->data_replicas)) {
				if (r->need_rb == BIT(BCH_RECONCILE_erasure_code))
					return bch2_extent_reconcile_pending_mod(trans, iter, level, k, true);
				/*
				 * Downstream rb-bit handling doesn't read the EC
				 * bit, so we don't need to clear it from r->need_rb
				 * (which is const). Just skip the EC action.
				 */
				goto skip_ec;
			}

			/* XXX: we'll need ratelimiting */
			if (extent_ec_pending(trans, ptrs))
				return false;

			data_opts->extra_replicas = 1;
			data_opts->no_devs_have = true;

			if (r->need_rb == BIT(BCH_RECONCILE_erasure_code))
				data_opts->write_flags |= BCH_WRITE_must_ec;
		} else {
			unsigned ptr_bit = 1;
			bkey_for_each_ptr_decode(k.k, ptrs, p, entry) {
				if (p.has_ec)
					data_opts->ptrs_kill_ec |= ptr_bit;

				ptr_bit <<= 1;
			}
		}
	}
skip_ec:

	scoped_guard(rcu) {
		unsigned ptr_bit = 1;
		bkey_for_each_ptr_decode(k.k, ptrs, p, entry) {
			if ((r->need_rb & BIT(BCH_RECONCILE_data_checksum)) &&
			    p.crc.csum_type != csum_type)
				data_opts->ptrs_kill |= ptr_bit;

			if ((r->need_rb & BIT(BCH_RECONCILE_background_compression)) &&
			    p.crc.compression_type != compression_type)
				data_opts->ptrs_kill |= ptr_bit;

			if ((r->need_rb & BIT(BCH_RECONCILE_background_target)) &&
			    !p.ptr.cached &&
			    !bch2_dev_in_target_rcu(c, p.ptr.dev, r->background_target))
				data_opts->ptrs_kill |= ptr_bit;

			ptr_bit <<= 1;
		}
	}

	bool ret = (data_opts->ptrs_kill ||
		    data_opts->ptrs_kill_ec ||
		    data_opts->extra_replicas);
	if (!ret) {
		if (r->need_rb == BIT(BCH_RECONCILE_data_replicas)) {
			/*
			 * We can end up here because you have all devices set
			 * to durability=2 and replicas set to 1, 3 - we can't
			 * exactly match the replicas setting - or because we
			 * want to drop replicas and we can't without reducing
			 * online durability
			 */
			return bch2_extent_reconcile_pending_mod(trans, iter, level, k, true);
		} else {
			CLASS(bch_log_msg_ratelimited, msg)(c);
			prt_printf(&msg.m, "got extent to reconcile but nothing to do, confused\n  ");
			bch2_bkey_val_to_text(&msg.m, c, k);
		}
	}

	return ret;
}

static void bkey_reconcile_pending_mod(struct bch_fs *c, struct bkey_i *k, bool set)
{
	struct bch_extent_reconcile *r = (struct bch_extent_reconcile *)
		bch2_bkey_reconcile_opts(c, bkey_i_to_s_c(k));
	BUG_ON(!r);

	r->pending = set;
}

int bch2_extent_reconcile_pending_mod(struct btree_trans *trans, struct btree_iter *iter,
				      unsigned level, struct bkey_s_c k, bool set)
{
	struct bch_fs *c = trans->c;

	const struct bch_extent_reconcile *r = bch2_bkey_reconcile_opts(c, k);
	if (!r || !r->need_rb) /* no work to do? */
		return 0;

	if ((rb_work_id(r) == RECONCILE_WORK_pending) == set)
		return 0;

	try(bch2_trans_relock(trans));

	unsigned buf_u64s = level ? BKEY_BTREE_PTR_U64s_MAX : BKEY_EXTENT_U64s_MAX;
	struct bkey_i *n = errptr_try(bch2_trans_kmalloc(trans, buf_u64s * sizeof(u64)));
	bkey_reassemble(n, k);

	if (!level) {
		bkey_reconcile_pending_mod(c, n, set);

		CLASS(disk_reservation, res)(c);
		return  bch2_trans_update_buf(trans, iter, n, buf_u64s, 0) ?:
			bch2_trans_commit(trans, &res.r, NULL,
					  BCH_TRANS_COMMIT_no_enospc);
	} else {
		CLASS(btree_node_iter, iter2)(trans, iter->btree_id, k.k->p, 0, level - 1, 0);
		struct btree *b = errptr_try(bch2_btree_iter_peek_node(&iter2));

		if (!btree_bkey_and_val_eq(bkey_i_to_s_c(&b->key), bkey_i_to_s_c(n))) {
			CLASS(printbuf, buf)();
			prt_newline(&buf);
			bch2_bkey_val_to_text(&buf, c, bkey_i_to_s_c(&b->key));
			prt_newline(&buf);
			bch2_bkey_val_to_text(&buf, c, k);
			panic("\n%s\n", buf.buf);
		}

		bkey_reconcile_pending_mod(c, n, set);

		return bch2_btree_node_update_key(trans, &iter2, b, n, BCH_TRANS_COMMIT_no_enospc, false);
	}
}

static int check_reconcile_pending_err(struct btree_trans *trans,
				       struct bch_inode_opts *opts,
				       struct data_update_opts *data_opts,
				       struct bkey_s_c k, int err)
{
	struct bch_fs *c = trans->c;

	 if (!bch2_err_matches(err, BCH_ERR_data_update_fail_no_rw_devs) &&
	     !bch2_err_matches(err, BCH_ERR_insufficient_devices) &&
	     !bch2_err_matches(err, ENOSPC))
		 return err;

	atomic64_inc(&c->reconcile.pending_reasons[bch2_move_outcome(err)]);

	s64 sectors = bkey_is_btree_ptr(k.k) ? btree_sectors(c) : k.k->size;

	event_add_trace(c, reconcile_set_pending, sectors, buf, ({
		prt_printf(&buf, "%s\n", bch2_err_str(err));
		bch2_bkey_val_to_text(&buf, c, k);
		prt_newline(&buf);
		bch2_data_update_opts_to_text(&buf, c, opts, data_opts);
		prt_newline(&buf);
		int ret = bch2_can_do_data_update(trans, opts, data_opts, k, &buf);
		if (bch2_err_matches(ret, BCH_ERR_transaction_restart))
			return ret;
	}));
	return 1;
}

typedef struct {
	u64		idx;
	u64		io_seq;
} stripe_retry;
DEFINE_DARRAY(stripe_retry);

static int do_reconcile_stripe(struct moving_context *ctxt,
			       struct btree_iter *iter,
			       struct bkey_s_c k,
			       darray_stripe_retry *retry)
{
	struct btree_trans *trans = ctxt->trans;
	struct bch_fs *c = trans->c;
	u32 restart_count = trans->restart_count;

	if (k.k->type != KEY_TYPE_stripe) /* write buffer race */
		return 0;

	struct bkey_s_c_stripe s = bkey_s_c_to_stripe(k);
	if (!s.v->needs_reconcile) /* write buffer race */
		return 0;

	struct bkey_buf stack_k __cleanup(bch2_bkey_buf_exit);
	bch2_bkey_buf_init(&stack_k);
	bch2_bkey_buf_reassemble(&stack_k, k);
	s = bkey_i_to_s_c_stripe(stack_k.k);;

	int ret = bch2_stripe_repair(ctxt, iter, s);

	event_add_trace(c, reconcile_stripe, le16_to_cpu(s.v->sectors) * s.v->nr_blocks, buf, ({
		bch2_bkey_val_to_text(&buf, c, s.s_c);
		prt_newline(&buf);
		prt_printf(&buf, "ret %s", bch2_err_str(ret));
	}));

	if (ret == -BCH_ERR_stripe_needs_block_evacuate) {
		if (retry) {
			darray_push(retry, ((stripe_retry) {
					    .idx	= k.k->p.offset,
					    .io_seq	= ctxt->io_seq,
			}));
		} else {
			CLASS(bch_log_msg_ratelimited, msg)(c);
			prt_printf(&msg.m, "error retrying stripe: %s\n", bch2_err_str(ret));
			bch2_bkey_val_to_text(&msg.m, c, s.s_c);
		}
		ret = 0;
	}

	/* Suppress trans_was_restarted() check */
	trans->restart_count = restart_count;
	return ret;
}

static bool stripe_retry_must_wait(struct moving_context *ctxt,
				   u64 stripe_io_seq)
{
	guard(mutex)(&ctxt->lock);
	struct data_update *u = !list_empty(&ctxt->ios)
		? list_last_entry(&ctxt->ios, struct data_update, io_list)
		: NULL;

	return u && u->io_seq <= stripe_io_seq;
}

static int do_retry_stripe(struct moving_context *ctxt, u64 idx)
{
	struct btree_trans *trans = ctxt->trans;

	CLASS(btree_iter, iter)(trans, BTREE_ID_stripes, POS(0, idx), BTREE_ITER_cached|BTREE_ITER_intent);
	struct bkey_s_c k = bkey_try(bch2_btree_iter_peek_slot(&iter));

	return do_reconcile_stripe(ctxt, &iter, k, NULL);
}

static int do_retry_stripes(struct moving_context *ctxt,
			    darray_stripe_retry *retry)
{
	struct btree_trans *trans = ctxt->trans;

	darray_for_each(*retry, i) {
		if (stripe_retry_must_wait(ctxt, i->io_seq)) {
			darray_remove_items(retry, retry->data, i - retry->data);
			return 0;
		}

		try(lockrestart_do(trans, do_retry_stripe(ctxt, i->idx)));
	}

	retry->nr = 0;
	return 0;
}

static int __do_reconcile_extent(struct moving_context *ctxt,
				 struct per_snapshot_io_opts *snapshot_io_opts,
				 struct bch_inode_opts *opts,
				 struct data_update_opts *data_opts,
				 struct bbpos work,
				 struct btree_iter *iter,
				 unsigned level,
				 struct bkey_s_c k,
				 darray_stripe_retry *stripe_retry)
{
	if (k.k->type == KEY_TYPE_stripe)
		return do_reconcile_stripe(ctxt, iter, k, stripe_retry);

	if (!bkey_extent_is_direct_data(k.k))
		return 0;

	struct btree_trans *trans = ctxt->trans;
	struct bch_fs *c = trans->c;
	u32 restart_count = trans->restart_count;

	ctxt->stats = &c->reconcile.work_stats;

	try(bch2_bkey_get_io_opts(trans, snapshot_io_opts, k, opts));
	try(bch2_update_reconcile_opts(trans, snapshot_io_opts, opts, iter, level, k,
				       SET_NEEDS_RECONCILE_other));

	/*
	 * The reconcile opts have to be committed here, not carried into the
	 * data update: bch2_move_extent() below unlocks to start the IO, and
	 * past that the transaction is no longer idempotent - there is no
	 * commit left to make.
	 *
	 * But not bch2_trans_commit_lazy(), which signals success as
	 * transaction_restart_commit. do_reconcile_extent_phys() drives us from
	 * a scan of the backpointers btree, and a write buffer btree scan
	 * cannot see the keys its own commit just buffered - so the restart
	 * re-reads the same position, finds the same work, and never
	 * terminates.
	 *
	 * A commit invalidates the pointers peek() handed out, so normally we
	 * couldn't keep using @k past this point - but every caller passes a
	 * bch2_bkey_buf_reassemble()d copy rather than a pointer into a btree
	 * node, so there's nothing here for the commit to invalidate.
	 */
	CLASS(disk_reservation, res)(c);
	try(bch2_trans_commit(trans, &res.r, NULL, BCH_TRANS_COMMIT_no_enospc));

	int ret = reconcile_set_data_opts(trans, iter, level, k, opts, data_opts);
	if (ret <= 0)
		return ret;

	if (work.btree == BTREE_ID_reconcile_pending) {
		int ret = bch2_can_do_data_update(trans, opts, data_opts, k, NULL);
		ret = check_reconcile_pending_err(trans, opts, data_opts, k, ret);
		if (ret > 0)
			return 0;
		if (ret)
			return ret;

		if (extent_has_rotational(c, k)) {
			/*
			 * The pending list is in logical inode:offset order,
			 * but if the extent is on spinning rust we want do it
			 * in device LBA order.
			 *
			 * Just take it off the pending list for now, and we'll
			 * pick it up when we scan reconcile_work_phys:
			 */
			return bch2_extent_reconcile_pending_mod(trans, iter, level, k, false);
		}
	}

	ret = bch2_move_extent(ctxt, NULL, opts, data_opts, iter, level, k);
	BUG_ON(ret > 0);
	ret = check_reconcile_pending_err(trans, opts, data_opts, k, ret);
	if (ret > 0)
		return bch2_extent_reconcile_pending_mod(trans, iter, level, k, true);
	if (bch2_err_matches(ret, BCH_ERR_transaction_restart) ||
	    bch2_err_matches(ret, BCH_ERR_data_update_fail_need_copygc))
		return ret;
	if (ret) {
		WARN_ONCE(!bch2_err_matches(ret, EROFS) &&
			  !bch2_err_matches(ret, BCH_ERR_snapshot) &&
			  !bch2_err_matches(ret, BCH_ERR_data_update_fail_no_snapshot) &&
			  !bch2_err_matches(ret, BCH_ERR_data_update_fail_in_flight) &&
			  !bch2_err_matches(ret, BCH_ERR_freelist_empty) &&
			  !bch2_err_matches(ret, BCH_ERR_open_buckets_empty),
			  "unhandled error from move_extent: %s", bch2_err_str(ret));
		/* skip it and continue */
	}

	/*
	 * Suppress trans_was_restarted() check: read_extent -> ec retry will
	 * handle transaction restarts, and we don't care:
	 */
	trans->restart_count = restart_count;
	return 0;
}

static int do_reconcile_extent(struct moving_context *ctxt,
			       struct per_snapshot_io_opts *snapshot_io_opts,
			       struct bbpos work,
			       darray_stripe_retry *stripe_retry)
{
	struct btree_trans *trans = ctxt->trans;
	struct bch_fs *c = trans->c;
	struct bbpos data_pos = rb_work_to_data_pos(work.pos);

	/* We require holding an intent lock when calling
	 * bch2_stripe_handle_tryget(), to avoid racing with the stripe trigger
	 * deleting the stripe */
	enum btree_iter_update_trigger_flags flags = data_pos.btree == BTREE_ID_stripes
		? BTREE_ITER_intent : 0;

	CLASS(btree_iter, iter)(trans, data_pos.btree, data_pos.pos, BTREE_ITER_all_snapshots|flags);
	struct bkey_s_c k = bkey_try(bch2_btree_iter_peek_slot(&iter));
	if (!k.k)
		return 0;

	struct bkey_buf stack_k __cleanup(bch2_bkey_buf_exit);
	bch2_bkey_buf_init(&stack_k);
	bch2_bkey_buf_reassemble(&stack_k, k);

	struct bch_inode_opts opts;
	struct data_update_opts data_opts = {
		.reconcile_phase	= c->reconcile.phase,
	};
	try(__do_reconcile_extent(ctxt, snapshot_io_opts, &opts, &data_opts,
				  work, &iter, 0,
				  bkey_i_to_s_c(stack_k.k), stripe_retry));

	event_add_trace(c, reconcile_data, stack_k.k->k.size, buf, ({
		prt_newline(&buf);
		bch2_bkey_val_to_text(&buf, c, bkey_i_to_s_c(stack_k.k));
		prt_newline(&buf);
		bch2_data_update_opts_to_text(&buf, c, &opts, &data_opts);
	}));
	return 0;
}

/*
 * Destage prepass filter: the extent needs background_target work and one of
 * the pointers that has to move is on a non-rotational device - return the
 * device to read from; -1 means not destage work.
 *
 * With copies on several SSDs, read from the one with the fewest move reads
 * in flight, then the lowest read latency: always taking the first pointer
 * piled destage reads onto one device while the other idled.
 *
 * Reads the reconcile entry stored in the extent, which may predate an option
 * change: a key misjudged here is still handled by the normal logical phase.
 */
static int reconcile_destage_read_dev(struct bch_fs *c, struct bkey_s_c k)
{
	const struct bch_extent_reconcile *r = bch2_bkey_reconcile_opts(c, k);
	if (!r || !(r->need_rb & BIT(BCH_RECONCILE_background_target)))
		return -1;

	struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(k);
	const union bch_extent_entry *entry;
	struct extent_ptr_decoded p;
	unsigned ptr_bit = 1;
	int best = -1;
	unsigned best_reads = 0;
	u64 best_latency = 0;

	guard(rcu)();
	bkey_for_each_ptr_decode(k.k, ptrs, p, entry) {
		if (r->ptrs_moving & ptr_bit) {
			struct bch_dev *ca = bch2_dev_rcu_noerror(c, p.ptr.dev);
			if (ca && !ca->mi.rotational) {
				unsigned reads	= atomic_read(&ca->move_reads_in_flight);
				u64 latency	= atomic64_read(&ca->cur_latency[READ]);

				if (best < 0 ||
				    reads < best_reads ||
				    (reads == best_reads && latency < best_latency)) {
					best		= p.ptr.dev;
					best_reads	= reads;
					best_latency	= latency;
				}
			}
		}
		ptr_bit <<= 1;
	}
	return best;
}

/*
 * Returns 1 if the entry isn't destage work: it is skipped without touching
 * the extent or its work entry. Checked before __do_reconcile_extent(), which
 * commits updated reconcile opts.
 */
static int do_reconcile_extent_destage(struct moving_context *ctxt,
				       struct per_snapshot_io_opts *snapshot_io_opts,
				       struct bbpos work,
				       darray_stripe_retry *stripe_retry)
{
	struct btree_trans *trans = ctxt->trans;
	struct bch_fs *c = trans->c;
	struct bbpos data_pos = rb_work_to_data_pos(work.pos);

	if (data_pos.btree == BTREE_ID_stripes)
		return 1;

	CLASS(btree_iter, iter)(trans, data_pos.btree, data_pos.pos, BTREE_ITER_all_snapshots);
	struct bkey_s_c k = bkey_try(bch2_btree_iter_peek_slot(&iter));
	if (!k.k)
		return 1;

	int read_dev = reconcile_destage_read_dev(c, k);
	if (read_dev < 0)
		return 1;

	/*
	 * Admission control on the source device, before the data update
	 * takes any locks; drops the btree locks and returns a restart if
	 * relocking fails, retrying from the lookup above:
	 */
	try(bch2_move_wait_dev_reads(ctxt, read_dev));

	struct bkey_buf stack_k __cleanup(bch2_bkey_buf_exit);
	bch2_bkey_buf_init(&stack_k);
	bch2_bkey_buf_reassemble(&stack_k, k);

	struct bch_inode_opts opts;
	struct data_update_opts data_opts = {
		.read_dev		= read_dev,
		.read_flags		= BCH_READ_soft_require_read_device,
		.reconcile_phase	= c->reconcile.phase,
	};
	try(__do_reconcile_extent(ctxt, snapshot_io_opts, &opts, &data_opts,
				  work, &iter, 0,
				  bkey_i_to_s_c(stack_k.k), stripe_retry));

	event_add_trace(c, reconcile_data, stack_k.k->k.size, buf, ({
		prt_newline(&buf);
		bch2_bkey_val_to_text(&buf, c, bkey_i_to_s_c(stack_k.k));
		prt_newline(&buf);
		bch2_data_update_opts_to_text(&buf, c, &opts, &data_opts);
	}));
	return 0;
}

static int do_reconcile_extent_phys(struct moving_context *ctxt,
				    struct per_snapshot_io_opts *snapshot_io_opts,
				    unsigned reconcile_phase,
				    struct bbpos work,
				    struct wb_maybe_flush *last_flushed,
				    darray_stripe_retry *stripe_retry)
{
	struct btree_trans *trans = ctxt->trans;
	struct bch_fs *c = trans->c;

	CLASS(btree_iter, bp_iter)(trans, BTREE_ID_backpointers, work.pos, 0);
	struct bkey_s_c bp_k = bkey_try(bch2_btree_iter_peek_slot(&bp_iter));
	if (!bp_k.k || bp_k.k->type != KEY_TYPE_backpointer) /* write buffer race */
		return 0;

	struct bkey_s_c_backpointer bp = bkey_s_c_to_backpointer(bp_k);

	struct bbpos pos = BBPOS(bp.v->btree_id, bp.v->pos);
	if (bch2_data_update_in_flight(c, &pos, BCH_DATA_UPDATE_reconcile))
		return 0;

	/* We require holding an intent lock when calling
	 * bch2_stripe_handle_tryget(), to avoid racing with the stripe trigger
	 * deleting the stripe */
	enum btree_iter_update_trigger_flags flags = bp.v->btree_id == BTREE_ID_stripes
		? BTREE_ITER_intent : 0;

	CLASS(btree_iter_uninit, iter)(trans);
	struct bkey_s_c k = bkey_try(bch2_backpointer_get_key(trans, bp, &iter, flags, last_flushed));
	if (!k.k)
		return 0;

	struct bkey_buf stack_bp __cleanup(bch2_bkey_buf_exit);
	bch2_bkey_buf_init(&stack_bp);
	bch2_bkey_buf_reassemble(&stack_bp, bp_k);

	struct bkey_buf stack_k __cleanup(bch2_bkey_buf_exit);
	bch2_bkey_buf_init(&stack_k);
	bch2_bkey_buf_reassemble(&stack_k, k);

	struct bch_inode_opts opts;
	struct data_update_opts data_opts = {
		.read_dev		= work.pos.inode,
		.read_flags		= BCH_READ_soft_require_read_device,
		.reconcile_phase	= reconcile_phase,
	};
	try(__do_reconcile_extent(ctxt, snapshot_io_opts, &opts,
				  &data_opts, work, &iter, bp.v->level,
				  bkey_i_to_s_c(stack_k.k), stripe_retry));

	event_add_trace(c, reconcile_phys, stack_k.k->k.size, buf, ({
		prt_newline(&buf);
		bch2_bkey_val_to_text(&buf, c, bkey_i_to_s_c(stack_bp.k));
		prt_newline(&buf);
		bch2_bkey_val_to_text(&buf, c, bkey_i_to_s_c(stack_k.k));
		prt_newline(&buf);
		bch2_data_update_opts_to_text(&buf, c, &opts, &data_opts);
	}));

	return 0;
}

noinline_for_stack
static int do_reconcile_btree(struct moving_context *ctxt,
			      struct per_snapshot_io_opts *snapshot_io_opts,
			      struct bbpos work,
			      struct bkey_s_c_backpointer bp)
{
	struct btree_trans *trans = ctxt->trans;
	struct bch_fs *c = trans->c;

	CLASS(btree_iter_uninit, iter)(trans);
	struct bkey_s_c k = bkey_try(reconcile_bp_get_key(trans, &iter, bp));
	if (!k.k)
		return 0;

	struct bkey_buf stack_k __cleanup(bch2_bkey_buf_exit);
	bch2_bkey_buf_init(&stack_k);
	bch2_bkey_buf_reassemble(&stack_k, k);

	struct bch_inode_opts opts;
	struct data_update_opts data_opts = {
		.reconcile_phase	= c->reconcile.phase,
	};
	try(__do_reconcile_extent(ctxt, snapshot_io_opts, &opts, &data_opts, work, &iter,
				  bp.v->level, bkey_i_to_s_c(stack_k.k), NULL));

	event_add_trace(c, reconcile_btree, btree_sectors(c), buf, ({
		prt_newline(&buf);
		bch2_bkey_val_to_text(&buf, c, bkey_i_to_s_c(stack_k.k));
		prt_newline(&buf);
		bch2_data_update_opts_to_text(&buf, c, &opts, &data_opts);
	}));

	return 0;
}

static int update_reconcile_opts_scan(struct btree_trans *trans,
				      struct per_snapshot_io_opts *snapshot_io_opts,
				      struct bch_inode_opts *opts,
				      struct btree_iter *iter,
				      unsigned level,
				      struct bkey_s_c k,
				      struct reconcile_scan s)
{
	switch (s.type) {
#define x(n) case RECONCILE_SCAN_##n:						\
		event_add_trace(trans->c, reconcile_scan_##n, !level ? k.k->size : btree_sectors(trans->c),	\
				buf, bch2_bkey_val_to_text(&buf, trans->c, k));	\
		break;
		RECONCILE_SCAN_TYPES()
#undef x
	}

	return bch2_update_reconcile_opts(trans, snapshot_io_opts, opts, iter, level, k,
					  SET_NEEDS_RECONCILE_opt_change);
}

static bool bch2_reconcile_enabled(struct bch_fs *c)
{
	return !c->opts.read_only &&
		c->opts.reconcile_enabled &&
		!(c->opts.reconcile_on_ac_only &&
		  c->reconcile.on_battery);
}

static int do_reconcile_scan_bp(struct btree_trans *trans,
				struct reconcile_scan s,
				struct bkey_s_c_backpointer bp,
				struct wb_maybe_flush *last_flushed)
{
	/*
	 * We're propagating a device state change or device durability change,
	 * and if an extent is erasure coded it'll be handled at the stripe
	 * level:
	 */
	if (BACKPOINTER_ERASURE_CODED(bp.v))
		return 0;

	struct bch_fs *c = trans->c;
	struct bch_fs_reconcile *r = &c->reconcile;

	CLASS(btree_iter_uninit, iter)(trans);
	struct bkey_s_c k = bkey_try(bch2_backpointer_get_key(trans, bp, &iter, BTREE_ITER_intent,
							      last_flushed));
	if (!k.k)
		return 0;

	atomic64_add(!bp.v->level ? k.k->size : c->opts.btree_node_size >> 9,
		     &r->scan_stats.sectors_seen);

	struct bch_inode_opts opts;
	try(bch2_bkey_get_io_opts(trans, NULL, k, &opts));

	return update_reconcile_opts_scan(trans, NULL, &opts, &iter, bp.v->level, k, s);
}

static int do_reconcile_scan_bps(struct moving_context *ctxt,
				 struct reconcile_scan s,
				 struct wb_maybe_flush *last_flushed)
{
	struct btree_trans *trans = ctxt->trans;
	struct bch_fs *c = trans->c;
	struct bch_fs_reconcile *r = &c->reconcile;

	r->scan_start	= BBPOS(BTREE_ID_backpointers, POS(s.dev, 0));
	r->scan_end	= BBPOS(BTREE_ID_backpointers, POS(s.dev, U64_MAX));

	return backpointer_scan_for_each(trans, iter, BTREE_ID_backpointers,
					 POS(s.dev, 0), POS(s.dev, U64_MAX),
				  last_flushed, NULL, bp, ({
		ctxt->stats->pos = BBPOS(BTREE_ID_backpointers, iter.pos);

		CLASS(disk_reservation, res)(c);
		(kthread_should_stop() || !bch2_reconcile_enabled(c)) ? 1 :
		do_reconcile_scan_bp(trans, s, bp, last_flushed) ?:
		bch2_trans_commit(trans, &res.r, NULL, BCH_TRANS_COMMIT_no_enospc);
	}));
}

static int do_reconcile_scan_indirect(struct moving_context *ctxt,
				      struct reconcile_scan s,
				      struct disk_reservation *res,
				      struct bkey_s_c_reflink_p p,
				      struct per_snapshot_io_opts *snapshot_io_opts,
				      struct bch_inode_opts *opts)
{
	struct btree_trans *trans = ctxt->trans;

	u64 idx = REFLINK_P_IDX(p.v) - le32_to_cpu(p.v->front_pad);
	u64 end = REFLINK_P_IDX(p.v) + p.k->size + le32_to_cpu(p.v->back_pad);
	u32 restart_count = trans->restart_count;

	try(for_each_btree_key_commit(trans, iter, BTREE_ID_reflink,
				      POS(0, idx),
				      BTREE_ITER_intent|
				      BTREE_ITER_not_extents, k,
				      res, NULL, BCH_TRANS_COMMIT_no_enospc, ({
		if (bpos_ge(bkey_start_pos(k.k), POS(0, end)))
			break;

		bch2_disk_reservation_put(trans->c, res);
		update_reconcile_opts_scan(trans, snapshot_io_opts, opts, &iter, 0, k, s);
	})));

	/* suppress trans_was_restarted() check */
	trans->restart_count = restart_count;
	return 0;
}

static int do_reconcile_scan_btree(struct moving_context *ctxt,
				   struct reconcile_scan s,
				   struct per_snapshot_io_opts *snapshot_io_opts,
				   enum btree_id btree, unsigned level,
				   struct bpos start, struct bpos end)
{
	struct btree_trans *trans = ctxt->trans;
	struct bch_fs *c = trans->c;
	struct bch_fs_reconcile *r = &c->reconcile;

	try(for_btree_root_key_at_level(trans, iter, btree, level, k, ({
		struct bch_inode_opts opts;
		bch2_bkey_get_io_opts(trans, snapshot_io_opts, k, &opts) ?:
		update_reconcile_opts_scan(trans, snapshot_io_opts, &opts, &iter, level, k, s);
	})));

	bch2_trans_begin(trans);
	CLASS(btree_node_iter, iter)(trans, btree, start, 0, level,
				     BTREE_ITER_prefetch|
				     BTREE_ITER_not_extents|
				     BTREE_ITER_all_snapshots);
	CLASS(disk_reservation, res)(c);

	return for_each_btree_key_max_continue(trans, iter, end, 0, k, ({
		ctxt->stats->pos = BBPOS(iter.btree_id, iter.pos);
		bch2_progress_update_iter(trans, &r->progress, &iter);

		atomic64_add(!level ? k.k->size : c->opts.btree_node_size >> 9,
			     &r->scan_stats.sectors_seen);

		bch2_disk_reservation_put(c, &res.r);

		struct bch_inode_opts opts;
		(kthread_should_stop() || !bch2_reconcile_enabled(c)) ? 1 :
		bch2_bkey_get_io_opts(trans, snapshot_io_opts, k, &opts) ?:
		update_reconcile_opts_scan(trans, snapshot_io_opts, &opts, &iter, level, k, s) ?:
		(start.inode &&
		 k.k->type == KEY_TYPE_reflink_p &&
		 REFLINK_P_MAY_UPDATE_OPTIONS(bkey_s_c_to_reflink_p(k).v)
		 ? do_reconcile_scan_indirect(ctxt, s, &res.r, bkey_s_c_to_reflink_p(k),
					      snapshot_io_opts, &opts)
		 : 0) ?:
		bch2_trans_commit(trans, &res.r, NULL, BCH_TRANS_COMMIT_no_enospc);
	}));
}

static int do_reconcile_scan_fs(struct moving_context *ctxt, struct reconcile_scan s,
				struct per_snapshot_io_opts *snapshot_io_opts,
				bool metadata)
{
	struct bch_fs *c = ctxt->trans->c;
	struct bch_fs_reconcile *r = &c->reconcile;

	bch2_progress_init(&r->progress, NULL, c, metadata ? 0 : ~0ULL, ~0ULL);

	r->scan_start	= BBPOS_MIN;
	r->scan_end	= BBPOS_MAX;

	for (enum btree_id btree = 0; btree < btree_id_nr_alive(c); btree++) {
		if (!bch2_btree_id_root(c, btree)->b)
			continue;

		bool scan_leaves = !metadata &&
			(btree == BTREE_ID_extents ||
			 btree == BTREE_ID_reflink);

		for (unsigned level = !scan_leaves; level < BTREE_MAX_DEPTH; level++)
			try(do_reconcile_scan_btree(ctxt, s, snapshot_io_opts,
						    btree, level, POS_MIN, SPOS_MAX));
	}

	return 0;
}

static int reconcile_scan_stripe_can_widen_one(struct btree_trans *trans,
					       struct btree_iter *iter,
					       struct bkey_s_c k,
					       widen_cache *cache)
{
	struct bch_fs *c = trans->c;

	if (k.k->type != KEY_TYPE_stripe)
		return 0;

	const struct bch_stripe *cur = bkey_s_c_to_stripe(k).v;
	unsigned nr_devs;
	try(bch2_widen_cache_lookup(cache, c,
				    cur->disk_label, le16_to_cpu(cur->sectors),
				    &nr_devs));

	u8 new_can_widen = stripe_widen_value(
		stripe_widen_target_nr_data(nr_devs, cur->nr_redundant,
					    c->opts.ec_max_data_blocks),
		cur->nr_blocks - cur->nr_redundant);

	if (cur->can_widen == new_can_widen)
		return 0;

	struct bkey_i_stripe *update =
		errptr_try(bch2_bkey_make_mut_typed(trans, iter, &k, 0, stripe));
	update->v.can_widen = new_can_widen;
	return 0;
}

static int do_reconcile_scan_stripes(struct moving_context *ctxt)
{
	struct btree_trans *trans = ctxt->trans;
	struct bch_fs *c = trans->c;
	struct bch_fs_reconcile *r = &c->reconcile;

	CLASS(widen_cache, cache)();
	try(bch2_widen_cache_init(&cache));

	bch2_progress_init(&r->progress, NULL, c, BIT_ULL(BTREE_ID_stripes), 0);
	r->scan_start	= BBPOS(BTREE_ID_stripes, POS_MIN);
	r->scan_end	= BBPOS(BTREE_ID_stripes, SPOS_MAX);

	return for_each_btree_key_commit(trans, iter, BTREE_ID_stripes,
			POS_MIN, BTREE_ITER_prefetch, k,
			NULL, NULL, BCH_TRANS_COMMIT_no_enospc, ({
		ctxt->stats->pos = BBPOS(iter.btree_id, iter.pos);
		bch2_progress_update_iter(trans, &r->progress, &iter);

		atomic64_add(c->opts.btree_node_size >> 9,
			     &r->scan_stats.sectors_seen);

		(kthread_should_stop() || !bch2_reconcile_enabled(c)) ? 1 :
		reconcile_scan_stripe_can_widen_one(trans, &iter, k, &cache);
	}));
}

noinline_for_stack
static int do_reconcile_scan(struct moving_context *ctxt,
			     struct per_snapshot_io_opts *snapshot_io_opts,
			     struct bpos cookie_pos, u64 cookie, u64 *sectors_scanned,
			     struct wb_maybe_flush *last_flushed)
{
	struct btree_trans *trans = ctxt->trans;
	struct bch_fs *c = trans->c;
	struct bch_fs_reconcile *r = &c->reconcile;

	/*
	 * If an opt change is still mid-flight for this cookie we couldn't
	 * complete the scan anyway - bch2_clear_reconcile_needs_scan() would
	 * refuse to delete the cookie - so don't burn a full pass on it;
	 * bch2_set_reconcile_needs_scan_post()'s wakeup brings us back once the
	 * change settles.
	 */
	if (reconcile_scan_in_flight(c, cookie_pos.offset))
		return 0;

	bch2_move_stats_init(&r->scan_stats, "reconcile_scan");
	ctxt->stats = &r->scan_stats;

	struct reconcile_scan s = reconcile_scan_decode(c, cookie_pos.offset);
	if (s.type == RECONCILE_SCAN_fs) {
		try(do_reconcile_scan_fs(ctxt, s, snapshot_io_opts, false));
	} else if (s.type == RECONCILE_SCAN_metadata) {
		try(do_reconcile_scan_fs(ctxt, s, snapshot_io_opts, true));
	} else if (s.type == RECONCILE_SCAN_device) {
		try(do_reconcile_scan_bps(ctxt, s, last_flushed));
	} else if (s.type == RECONCILE_SCAN_stripes) {
		try(do_reconcile_scan_stripes(ctxt));
	} else if (s.type == RECONCILE_SCAN_inum) {
		r->scan_start	= BBPOS(BTREE_ID_extents, POS(s.inum, 0));
		r->scan_end	= BBPOS(BTREE_ID_extents, POS(s.inum, U64_MAX));

		try(do_reconcile_scan_btree(ctxt, s, snapshot_io_opts, BTREE_ID_extents, 0,
					    r->scan_start.pos, r->scan_end.pos));
	}

	try(bch2_clear_reconcile_needs_scan(trans, cookie_pos, cookie));

	*sectors_scanned += atomic64_read(&r->scan_stats.sectors_seen);
	/*
	 * Ensure that the entries we created are seen by the next iteration of
	 * do_reconcile(), so we don't end up stuck in reconcile_wait():
	 */
	*sectors_scanned += 1;
	bch2_move_stats_exit(&r->scan_stats, c);
	return 0;
}

static bool reconcile_hipri_work_pending(struct bch_fs *c)
{
	struct disk_accounting_pos pos;
	disk_accounting_key_init(pos, reconcile_work,
				 BCH_RECONCILE_ACCOUNTING_high_priority);

	u64 v[2];
	bch2_accounting_mem_read(c, disk_accounting_pos_to_bpos(&pos), v, ARRAY_SIZE(v));
	return v[0] || v[1];
}

static void reconcile_wait(struct bch_fs *c, u32 kick)
{
	struct bch_fs_reconcile *r = &c->reconcile;
	struct io_clock *clock = &c->io_clock[WRITE];
	u64 now = atomic64_read(&clock->now);
	u64 min_member_capacity = bch2_min_rw_member_capacity(c);

	if (reconcile_hipri_work_pending(c)) {
		cond_resched();
		return;
	}

	if (min_member_capacity == U64_MAX)
		min_member_capacity = 128 * 2048;

	r->wait_iotime_end		= now + (min_member_capacity >> 6);

	if (r->running) {
		r->wait_iotime_start	= now;
		r->wait_wallclock_start	= ktime_get_real_ns();
		r->running		= false;
	}

	/*
	 * Recheck the kick after setting TASK_INTERRUPTIBLE: a kick +
	 * wake_up_process() is either seen here or wakes the sleep - no lost
	 * wakeups:
	 */
	set_current_state(TASK_INTERRUPTIBLE);
	if (kick == READ_ONCE(r->kick))
		bch2_kthread_io_clock_wait_once(clock, r->wait_iotime_end, MAX_SCHEDULE_TIMEOUT);
	__set_current_state(TASK_RUNNING);
}

struct reconcile_phase {
	enum reconcile_phase_type	type;
	enum reconcile_work_id		priority;
	enum btree_id			btree;
	struct bpos			start, end;
};

static const struct reconcile_phase reconcile_phases[] = {
	/* Scan cookies: */
	{ RECONCILE_PHASE_scan,		RECONCILE_WORK_hipri,
		BTREE_ID_reconcile_scan, POS_MIN, POS(0, U64_MAX), },

	/* Hipri work first - evacuate/rereplicate */

	/*
	 * Btree nodes first - they're indexed separately from the normal work
	 * btrees because they require backpointers:
	 */
	{ RECONCILE_PHASE_btree,	RECONCILE_WORK_hipri,
		BTREE_ID_reconcile_scan, POS(RECONCILE_WORK_hipri, 0), POS(RECONCILE_WORK_hipri, U64_MAX) },

	/*
	 * User data:
	 * Phys btrees first: pending work there will also be present in the normal work btrees
	 * Then the logical btrees, this will be data on SSDS:
	 * */
	{ RECONCILE_PHASE_phys,		RECONCILE_WORK_hipri,
		BTREE_ID_reconcile_hipri_phys,	POS_MIN, SPOS_MAX },
	{ RECONCILE_PHASE_normal,	RECONCILE_WORK_hipri,
		BTREE_ID_reconcile_hipri,		POS_MIN, SPOS_MAX },

	/* Normal priority work: */
	{ RECONCILE_PHASE_btree,	RECONCILE_WORK_normal,
		BTREE_ID_reconcile_scan, POS(RECONCILE_WORK_normal, 0), POS(RECONCILE_WORK_normal, U64_MAX) },

	/*
	 * Destage prepass: background_target moves of data sitting on
	 * non-rotational devices. Otherwise these only happen in the normal
	 * logical phase, after the phys phase - which runs to exhaustion and can
	 * take days on a large rotational backlog while the SSDs fill.
	 *
	 * Filtered sweep of the logical work btree: entries that don't match are
	 * stepped over and left untouched for the phases below. The phys phase
	 * can't do this work, rotational-device pointers are all it indexes.
	 */
	{ RECONCILE_PHASE_destage,	RECONCILE_WORK_normal,
		BTREE_ID_reconcile_work,		POS_MIN, SPOS_MAX },

	{ RECONCILE_PHASE_phys,		RECONCILE_WORK_normal,
		BTREE_ID_reconcile_work_phys,		POS_MIN, SPOS_MAX },
	{ RECONCILE_PHASE_normal,	RECONCILE_WORK_normal,
		BTREE_ID_reconcile_work,		POS_MIN, SPOS_MAX },

	/*
	 * Lastly, work that we marked as unable to complete until system
	 * configuration changes: this won't be process unless kicked by
	 * something else
	 */
	{ RECONCILE_PHASE_btree,	RECONCILE_WORK_pending,
		BTREE_ID_reconcile_scan, POS(RECONCILE_WORK_pending, 0), POS(RECONCILE_WORK_pending, U64_MAX) },
	{ RECONCILE_PHASE_normal,	RECONCILE_WORK_pending,
		BTREE_ID_reconcile_pending,		POS_MIN, SPOS_MAX },
};

/*
 * A run of this many need_copygc deferrals ends a keyed phase for this pass:
 * each one waited for a copygc run already, and a phase where nothing can be
 * placed shouldn't hold up the phases after it.
 */
#define RECONCILE_MAX_CONSECUTIVE_DEFERRED	16

static struct bpos reconcile_work_pos_successor(struct bbpos pos)
{
	return btree_type_has_snapshot_field(pos.btree)
		? bpos_successor(pos.pos)
		: bpos_nosnap_successor(pos.pos);
}

static struct bpos reconcile_work_pos_predecessor(struct bbpos pos)
{
	return btree_type_has_snapshot_field(pos.btree)
		? bpos_predecessor(pos.pos)
		: bpos_nosnap_predecessor(pos.pos);
}

typedef struct {
	struct bch_fs		*c;
	unsigned		dev;
	unsigned		reconcile_phase;
	u32			kick;
	u64			deadline;
	struct closure		cl;

	struct bch_move_stats	stats;

	enum reconcile_phase_exit exit;
	u64			deferred;
} reconcile_phys_thr;

static struct reconcile_lap *reconcile_phys_lap(struct bch_fs *c, unsigned dev,
						unsigned reconcile_phase)
{
	return &c->reconcile.phys_laps[dev * 2 +
		(reconcile_phases[reconcile_phase].priority != RECONCILE_WORK_hipri)];
}

DEFINE_DARRAY(reconcile_phys_thr);

/*
 * Destructor ordering: closure_return() must be the last thing before the
 * function returns, but __cleanup destructors run after closure_return()
 * signals the parent — which can then free the thrs darray containing the
 * reconcile_phys_thr (and its embedded bch_move_stats) that
 * moving_context.stats still points to. So we manage moving_context
 * lifetime manually here.
 *
 * This is a general hazard with __cleanup + closure_return: the parent
 * can wake and free resources before the child's destructors run. In Rust
 * this will be enforced by Drop ordering.
 */
static CLOSURE_CALLBACK(do_reconcile_phys_thread)
{
	closure_type(thr, reconcile_phys_thr, cl);
	struct bch_fs *c = thr->c;

	struct moving_context ctxt;
	bch2_moving_ctxt_init(&ctxt, c, NULL, &thr->stats,
			      writepoint_ptr(&c->allocator.reconcile_write_point),
			      true);
	bch2_moving_ctxt_set_budget(&ctxt, &c->reconcile.move_budget);
	ctxt.metadata_throttle = true;

	struct btree_trans *trans = ctxt.trans;

	CLASS(darray_reconcile_work, work)();
	darray_make_room(&work, RECONCILE_WORK_BUF_NR);
	if (!work.size) {
		bch_err(c, "%s: unable to allocate memory", __func__);
		bch2_moving_ctxt_exit(&ctxt);
		closure_return(cl);
		return;
	}

	CLASS(per_snapshot_io_opts, snapshot_io_opts)(c);

	CLASS(darray_stripe_retry, stripe_retry)();

	struct wb_maybe_flush last_flushed __cleanup(wb_maybe_flush_exit);
	wb_maybe_flush_init(&last_flushed);

	/*
	 * This device's lap: LBA order from wherever the last one stopped,
	 * wrapping to the start of the device for the part before it.
	 */
	enum btree_id btree = reconcile_phases[thr->reconcile_phase].btree;
	struct bpos range_start = POS(thr->dev, 0);
	struct reconcile_lap *lap = reconcile_phys_lap(c, thr->dev, thr->reconcile_phase);

	if (!lap->active)
		*lap = (struct reconcile_lap) {
			.cursor	= range_start,
			.start	= range_start,
			.active	= true,
		};

	struct bbpos work_pos = BBPOS(btree, lap->cursor);
	struct bpos resume = lap->cursor;
	unsigned consecutive_deferred = 0;
	u32 copygc_run_count = c->copygc.run_count;

	thr->exit = RECONCILE_PHASE_EXIT_stopped;

	while (!bch2_move_ratelimit(&ctxt)) {
		if (!bch2_reconcile_enabled(c) ||
		    test_bit(BCH_FS_going_ro, &c->flags))
			break;

		if (thr->kick != READ_ONCE(c->reconcile.kick)) {
			thr->exit = RECONCILE_PHASE_EXIT_kick;
			break;
		}

		if (thr->deadline && ktime_get_ns() > thr->deadline) {
			thr->exit = RECONCILE_PHASE_EXIT_yield;
			break;
		}

		bch2_trans_begin(trans);

		struct bpos end = lap->wrapped
			? reconcile_work_pos_predecessor(BBPOS(btree, lap->start))
			: POS(thr->dev, U64_MAX);
		struct bkey_s_c k = next_reconcile_entry(trans, &work, &work_pos, end);
		if (bkey_err(k)) {
			thr->exit = RECONCILE_PHASE_EXIT_error;
			break;
		}

		if (!k.k || k.k->p.inode != thr->dev) {
			if (!lap->wrapped && bpos_gt(lap->start, range_start)) {
				lap->wrapped	= true;
				work_pos.pos	= range_start;
				resume		= range_start;
				continue;
			}

			lap->active = false;
			thr->exit = RECONCILE_PHASE_EXIT_exhausted;
			break;
		}

		struct bpos pos = k.k->p;

		int ret = lockrestart_do(trans,
			do_reconcile_extent_phys(&ctxt, &snapshot_io_opts,
						 thr->reconcile_phase,
						 BBPOS(btree, pos),
						 &last_flushed,
						 &stripe_retry));

		if (bch2_err_matches(ret, BCH_ERR_data_update_fail_need_copygc)) {
			/*
			 * As in do_reconcile_phase_iter(): flush our moves
			 * before waiting (they hold nocow locks copygc needs),
			 * then step past the entry instead of retrying it.
			 * Bounded: this is a workqueue worker, and every
			 * device's worker holds up the pass.
			 */
			bch2_moving_ctxt_flush_all(&ctxt);
			bch2_copygc_wakeup(c);
			wait_event_timeout(c->copygc.running_wq,
				   c->copygc.run_count != copygc_run_count ||
				   test_bit(BCH_FS_going_ro, &c->flags),
				   10 * HZ);
			copygc_run_count = c->copygc.run_count;
			thr->deferred++;

			resume = reconcile_work_pos_successor(BBPOS(btree, pos));
			if (++consecutive_deferred >= RECONCILE_MAX_CONSECUTIVE_DEFERRED) {
				thr->exit = RECONCILE_PHASE_EXIT_deferred_limit;
				break;
			}
			continue;
		}

		if (ret) {
			resume = pos;
			thr->exit = RECONCILE_PHASE_EXIT_error;
			break;
		}

		consecutive_deferred = 0;
		resume = reconcile_work_pos_successor(BBPOS(btree, pos));
	}

	if (lap->active)
		lap->cursor = resume;

	bch2_moving_ctxt_exit(&ctxt);
	closure_return(cl);
}

/*
 * How a phys phase ended, from its per-device workers: the most significant
 * worker exit wins - a phase is exhausted only if every device's lap is.
 */
static enum reconcile_phase_exit reconcile_phys_exit(darray_reconcile_phys_thr *thrs)
{
	static const enum reconcile_phase_exit order[] = {
		RECONCILE_PHASE_EXIT_error,
		RECONCILE_PHASE_EXIT_stopped,
		RECONCILE_PHASE_EXIT_kick,
		RECONCILE_PHASE_EXIT_yield,
		RECONCILE_PHASE_EXIT_deferred_limit,
	};

	for (unsigned i = 0; i < ARRAY_SIZE(order); i++)
		darray_for_each(*thrs, t)
			if (t->exit == order[i])
				return order[i];
	return RECONCILE_PHASE_EXIT_exhausted;
}

static int do_reconcile_phys(struct bch_fs *c, unsigned reconcile_phase, u32 kick,
			     u64 deadline, enum reconcile_phase_exit *exit)
{
	CLASS(darray_reconcile_phys_thr, thrs)();
	CLASS(closure_stack, cl)();

	for_each_member_device(c, ca)
		if (ca->mi.rotational &&
		    bch2_dev_is_online(ca))
			try(darray_push(&thrs, ((reconcile_phys_thr) {
						.c			= c,
						.dev			= ca->dev_idx,
						.reconcile_phase	= reconcile_phase,
						.kick			= kick,
						.deadline		= deadline,
						})));

	darray_for_each(thrs, i)
		closure_call(&i->cl, do_reconcile_phys_thread, system_unbound_wq, &cl);

	closure_sync_unbounded(&cl);

	darray_for_each(thrs, i)
		c->reconcile.deferred += i->deferred;
	*exit = reconcile_phys_exit(&thrs);
	return 0;
}

/*
 * The long keyed phases resume where they were interrupted; the scan, btree
 * and pending phases are short, and restart from the beginning:
 */
static bool reconcile_phase_resumable(unsigned i)
{
	struct reconcile_phase p = reconcile_phases[i];

	return p.type == RECONCILE_PHASE_destage ||
		(p.type == RECONCILE_PHASE_normal && p.priority != RECONCILE_WORK_pending);
}

/* Inclusive end of the range the current lap still has to cover */
static struct bpos reconcile_phase_end(struct bch_fs_reconcile *r)
{
	struct reconcile_phase p = reconcile_phases[r->phase];
	struct reconcile_lap *lap = &r->laps[r->phase];

	if (!reconcile_phase_resumable(r->phase) || !lap->wrapped)
		return p.end;

	return reconcile_work_pos_predecessor(BBPOS(p.btree, lap->start));
}

/*
 * The range to the end is done: if the lap began partway in, go back to the
 * range start for the part before it. Returns false when the lap is complete.
 */
static bool reconcile_lap_wrap(struct bch_fs_reconcile *r)
{
	struct reconcile_phase p = reconcile_phases[r->phase];
	struct reconcile_lap *lap = &r->laps[r->phase];

	if (!reconcile_phase_resumable(r->phase))
		return false;

	if (!lap->wrapped && bpos_gt(lap->start, p.start)) {
		lap->wrapped = true;
		r->work_pos.pos = p.start;
		return true;
	}

	lap->active = false;
	r->laps_completed[r->phase]++;
	return false;
}

static void reconcile_phase_start(struct bch_fs *c)
{
	struct bch_fs_reconcile *r = &c->reconcile;
	struct reconcile_phase p = reconcile_phases[r->phase];
	struct reconcile_lap *lap = &r->laps[r->phase];

	if (reconcile_phase_resumable(r->phase)) {
		if (!lap->active) {
			*lap = (struct reconcile_lap) {
				.cursor		= p.start,
				.start		= p.start,
				.active		= true,
			};

			if (p.type == RECONCILE_PHASE_destage)
				r->destage_lap_attempted = 0;
		}

		r->work_pos = BBPOS(p.btree, lap->cursor);
	} else {
		r->work_pos = BBPOS(p.btree, p.start);
	}

	switch (p.type) {
	case RECONCILE_PHASE_normal:
	case RECONCILE_PHASE_destage:
		bch2_progress_init(&r->progress, NULL, c,
				   BIT_ULL(reconcile_work_btree[p.priority]), 0);
		break;
	case RECONCILE_PHASE_phys:
		bch2_progress_init(&r->progress, NULL, c,
				   BIT_ULL(reconcile_work_phys_btree[p.priority]), 0);
		break;
	default:
		break;
	}
}

static bool reconcile_phase_is_pending(unsigned i)
{
	struct reconcile_phase p = reconcile_phases[i];
	return (p.btree == BTREE_ID_reconcile_scan &&
		p.start.inode == RECONCILE_WORK_pending) ||
		p.btree == BTREE_ID_reconcile_pending;
}

/*
 * Per-pass cross-phase state. Threaded into do_reconcile_phase() so the
 * inner loop has all of it without a long parameter list.
 */
struct reconcile_pass {
	struct moving_context		*ctxt;
	struct per_snapshot_io_opts	*snapshot_io_opts;
	darray_reconcile_work		*work;
	struct wb_maybe_flush		*last_flushed;
	darray_stripe_retry		*stripe_retry;
	struct bkey_i_cookie		*pending_cookie;
	u64				*sectors_scanned;
	u32				*copygc_run_count;

	/* Set by the phase functions: why the last phase returned */
	enum reconcile_phase_exit	exit;

	/* ktime_get_ns() at which the current phase yields; 0: no limit */
	u64				deadline;
	/* A phase yielded with work left: don't wait at the end of the pass */
	bool				yielded;
	/* This pass's normal phys phase yielded; see do_reconcile_phase() */
	bool				phys_yielded;
};

/* Per-key handler: returns the result of processing one key in a keyed phase. */
typedef int (*reconcile_key_handler)(struct reconcile_pass *p, struct bkey_s_c k);

static int do_reconcile_scan_key(struct reconcile_pass *p, struct bkey_s_c k)
{
	struct btree_trans *trans = p->ctxt->trans;
	struct bch_fs *c = trans->c;

	if (reconcile_scan_decode(c, k.k->p.offset).type == RECONCILE_SCAN_pending)
		bkey_reassemble(&p->pending_cookie->k_i, k);

	int ret = do_reconcile_scan(p->ctxt, p->snapshot_io_opts, k.k->p,
				    le64_to_cpu(bkey_s_c_to_cookie(k).v->cookie),
				    p->sectors_scanned, p->last_flushed);

	if (bch2_err_matches(ret, BCH_ERR_transaction_restart)) {
#ifdef CONFIG_BCACHEFS_DEBUG
		CLASS(printbuf, buf)();
		bch2_prt_backtrace(&buf, &trans->last_restarted_trace);
		panic("in transaction restart: %s, last restarted by\n%s",
		      bch2_err_str(trans->restarted),
		      buf.buf);
#else
		panic("in transaction restart: %s, last restarted by %pS\n",
		      bch2_err_str(trans->restarted),
		      (void *) trans->last_restarted_ip);
#endif
	}
	return ret;
}

static int do_reconcile_btree_key(struct reconcile_pass *p, struct bkey_s_c k)
{
	struct bch_fs_reconcile *r = &p->ctxt->trans->c->reconcile;

	return do_reconcile_btree(p->ctxt, p->snapshot_io_opts, r->work_pos,
				  bkey_s_c_to_backpointer(k));
}

static int do_reconcile_extent_key(struct reconcile_pass *p, struct bkey_s_c k)
{
	struct btree_trans *trans = p->ctxt->trans;
	struct bch_fs_reconcile *r = &trans->c->reconcile;

	return lockrestart_do(trans,
		do_reconcile_extent(p->ctxt, p->snapshot_io_opts, r->work_pos,
				    p->stripe_retry));
}

static int do_reconcile_destage_key(struct reconcile_pass *p, struct bkey_s_c k)
{
	struct btree_trans *trans = p->ctxt->trans;
	struct bch_fs_reconcile *r = &trans->c->reconcile;

	int ret = lockrestart_do(trans,
		do_reconcile_extent_destage(p->ctxt, p->snapshot_io_opts, r->work_pos,
					    p->stripe_retry));
	if (ret > 0) {
		r->destage_skipped++;
		return 0;
	}

	r->destage_attempted++;
	r->destage_lap_attempted++;
	if (!ret)
		r->destage_completed++;
	return ret;
}

/*
 * Iterate one keyed phase (scan / btree / normal / destage) to exhaustion or
 * interrupt. The phys phase doesn't go through here — it's a one-shot,
 * dispatched separately from the outer loop.
 *
 * Returns 0 on phase done or interrupt, error on real failure. Caller
 * re-checks loop conditions to decide whether to advance or bail.
 */
static int do_reconcile_phase_iter(struct reconcile_pass *p, u32 kick,
				   reconcile_key_handler handler)
{
	struct moving_context *ctxt = p->ctxt;
	struct btree_trans *trans = ctxt->trans;
	struct bch_fs *c = trans->c;
	struct bch_fs_reconcile *r = &c->reconcile;
	unsigned consecutive_deferred = 0;
	int ret = 0;

	p->exit = RECONCILE_PHASE_EXIT_stopped;

	while (!bch2_move_ratelimit(ctxt) &&
	       !test_bit(BCH_FS_going_ro, &c->flags) &&
	       bch2_reconcile_enabled(c) &&
	       kick == r->kick) {
		if (p->deadline && ktime_get_ns() > p->deadline) {
			p->exit = RECONCILE_PHASE_EXIT_yield;
			break;
		}

		bch2_trans_begin(trans);

		struct bkey_s_c k = next_reconcile_entry(trans, p->work, &r->work_pos,
							 reconcile_phase_end(r));
		ret = bkey_err(k);
		if (ret) {
			p->exit = RECONCILE_PHASE_EXIT_error;
			break;
		}

		if (!k.k) {
			if (reconcile_lap_wrap(r))
				continue;

			p->exit = RECONCILE_PHASE_EXIT_exhausted;
			return 0;
		}

		r->work_pos.pos = k.k->p;

		ret = handler(p, k);

		if (bch2_err_matches(ret, BCH_ERR_data_update_fail_need_copygc)) {
			/*
			 * Flush our in-flight moves before sleeping: their
			 * writes are only ever issued from this thread, and
			 * each holds nocow locks until it completes. Copygc
			 * needs those locks to evacuate the buckets, so parking
			 * them here while waiting for copygc deadlocks.
			 */
			bch2_moving_ctxt_flush_all(ctxt);
			bch2_copygc_wakeup(c);
			wait_event(c->copygc.running_wq,
				   c->copygc.run_count != *p->copygc_run_count ||
				   kthread_should_stop());
			*p->copygc_run_count = c->copygc.run_count;
			ret = 0;

			/*
			 * Scan cookies are retried in place. Keyed work is
			 * deferred: step past the entry and leave its work
			 * entry for the next pass. Retrying in place let one
			 * entry that couldn't be placed stall every later
			 * phase - the btree phase re-reads from work_pos, so it
			 * got the same key back after every copygc run.
			 */
			if (reconcile_phases[r->phase].type == RECONCILE_PHASE_scan)
				continue;

			r->deferred++;
			if (reconcile_phases[r->phase].type == RECONCILE_PHASE_destage)
				r->destage_deferred++;

			if (++consecutive_deferred >= RECONCILE_MAX_CONSECUTIVE_DEFERRED) {
				r->work_pos.pos = reconcile_work_pos_successor(r->work_pos);
				p->exit = RECONCILE_PHASE_EXIT_deferred_limit;
				break;
			}
		} else if (bch2_err_matches(ret, BCH_ERR_transaction_restart)) {
			ret = 0;
			continue;
		} else if (ret) {
			p->exit = RECONCILE_PHASE_EXIT_error;
			break;
		} else {
			consecutive_deferred = 0;
			do_retry_stripes(ctxt, p->stripe_retry);
		}

		r->work_pos.pos = reconcile_work_pos_successor(r->work_pos);
	}

	if (p->exit == RECONCILE_PHASE_EXIT_stopped && kick != r->kick)
		p->exit = RECONCILE_PHASE_EXIT_kick;

	/* work_pos is past the last entry handled: resume there */
	if (reconcile_phase_resumable(r->phase))
		r->laps[r->phase].cursor = r->work_pos.pos;
	return ret;
}

/*
 * Phys phase: one-shot. do_reconcile_phys() fans out to per-device worker
 * threads which together consume the whole reconcile_*_phys btree, then we
 * return to advance to the next phase.
 */
static int do_reconcile_phase_phys(struct reconcile_pass *p, u32 kick)
{
	struct btree_trans *trans = p->ctxt->trans;
	struct bch_fs *c = trans->c;
	struct bch_fs_reconcile *r = &c->reconcile;

	bch2_trans_unlock_long(trans);
	int ret = do_reconcile_phys(c, r->phase, kick, p->deadline, &p->exit);
	BUG_ON(bch2_err_matches(ret, BCH_ERR_transaction_restart));
	return ret;
}

static bool reconcile_target_work_pending(struct bch_fs *c)
{
	struct disk_accounting_pos pos;
	disk_accounting_key_init(pos, reconcile_work,
				 BCH_RECONCILE_ACCOUNTING_target);

	u64 v[2];
	bch2_accounting_mem_read(c, disk_accounting_pos_to_bpos(&pos), v, ARRAY_SIZE(v));
	return v[0] || v[1];
}

/*
 * The normal priority destage, phys and logical phases take turns instead of
 * each running until empty, so destage recurs while a long phys backlog (EC
 * of rotational data) drains; hipri phases still run until empty.
 */
static u64 reconcile_phase_slice_ms(struct bch_fs *c, unsigned i)
{
	struct reconcile_phase p = reconcile_phases[i];

	if (p.priority != RECONCILE_WORK_normal)
		return 0;
	if (p.type == RECONCILE_PHASE_destage)
		return c->opts.reconcile_destage_slice_ms;
	if (p.type == RECONCILE_PHASE_phys ||
	    p.type == RECONCILE_PHASE_normal)
		return c->opts.reconcile_phase_slice_ms;
	return 0;
}

static int do_reconcile_phase(struct reconcile_pass *p, u32 kick)
{
	struct btree_trans *trans = p->ctxt->trans;
	struct bch_fs *c = trans->c;
	struct bch_fs_reconcile *r = &c->reconcile;
	struct reconcile_phase phase = reconcile_phases[r->phase];

	u64 slice_ms = reconcile_phase_slice_ms(c, r->phase);
	p->deadline = slice_ms ? ktime_get_ns() + slice_ms * NSEC_PER_MSEC : 0;

	/*
	 * Nothing for the filtered destage walk to find - or its last lap
	 * found nothing: target work it can't do (stuck entries, work on
	 * rotational devices) mustn't make it walk the work btree every pass.
	 */
	if (phase.type == RECONCILE_PHASE_destage &&
	    (!reconcile_target_work_pending(c) ||
	     ktime_get_ns() < r->destage_idle_until)) {
		p->exit = RECONCILE_PHASE_EXIT_skipped;
		return 0;
	}

	/*
	 * Rotational work is also indexed in the logical btree, but must be
	 * done in LBA order by the phys phase: while that has a lap in
	 * progress the logical phase would do it in inode:offset order.
	 */
	if (phase.type == RECONCILE_PHASE_normal &&
	    phase.priority == RECONCILE_WORK_normal &&
	    p->phys_yielded) {
		p->exit = RECONCILE_PHASE_EXIT_skipped;
		return 0;
	}

	bch2_btree_write_buffer_flush_sync(trans);

	switch (phase.type) {
	case RECONCILE_PHASE_scan:
		return do_reconcile_phase_iter(p, kick, do_reconcile_scan_key);
	case RECONCILE_PHASE_btree:
		return do_reconcile_phase_iter(p, kick, do_reconcile_btree_key);
	case RECONCILE_PHASE_phys:
		return do_reconcile_phase_phys(p, kick);
	case RECONCILE_PHASE_normal:
		return do_reconcile_phase_iter(p, kick, do_reconcile_extent_key);
	case RECONCILE_PHASE_destage: {
		u64 start = ktime_get_ns();
		int ret = do_reconcile_phase_iter(p, kick, do_reconcile_destage_key);

		r->destage_ns += ktime_get_ns() - start;
		r->destage_sweeps++;

		if (!ret &&
		    p->exit == RECONCILE_PHASE_EXIT_exhausted &&
		    !r->destage_lap_attempted)
			r->destage_idle_until = ktime_get_ns() + 10ULL * 60 * NSEC_PER_SEC;
		return ret;
	}
	default:
		BUG();
	}
}

static int do_reconcile(struct moving_context *ctxt)
{
	struct btree_trans *trans = ctxt->trans;
	struct bch_fs *c = trans->c;
	struct bch_fs_reconcile *r = &c->reconcile;
	u64 sectors_scanned = 0;
	u32 kick = r->kick;
	u32 copygc_run_count = c->copygc.run_count;
	int ret = 0;

	CLASS(darray_reconcile_work, work)();
	try(darray_make_room(&work, RECONCILE_WORK_BUF_NR));

	bch2_move_stats_init(&r->work_stats, "reconcile_work");

	CLASS(per_snapshot_io_opts, snapshot_io_opts)(c);

	struct bkey_i_cookie pending_cookie;
	bkey_init(&pending_cookie.k);

	bch2_moving_ctxt_flush_all(ctxt);

	struct wb_maybe_flush last_flushed __cleanup(wb_maybe_flush_exit);
	wb_maybe_flush_init(&last_flushed);

	CLASS(darray_stripe_retry, stripe_retry)();

	struct reconcile_pass pass = {
		.ctxt			= ctxt,
		.snapshot_io_opts	= &snapshot_io_opts,
		.work			= &work,
		.last_flushed		= &last_flushed,
		.stripe_retry		= &stripe_retry,
		.pending_cookie		= &pending_cookie,
		.sectors_scanned	= &sectors_scanned,
		.copygc_run_count	= &copygc_run_count,
	};

	r->running = true;

	while (!bch2_move_ratelimit(ctxt) &&
	       !test_bit(BCH_FS_going_ro, &c->flags)) {
		if (!bch2_reconcile_enabled(c)) {
			bch2_moving_ctxt_flush_all(ctxt);
			kthread_wait_freezable(bch2_reconcile_enabled(c) ||
					       kthread_should_stop());
			if (kthread_should_stop())
				break;
			continue;
		}

		/*
		 * Re-read kick: a kick during the previous pass restarts us
		 * here so do_reconcile_phase() sees the same kick the for-loop
		 * compares against — and the post-loop reconcile_wait() check
		 * uses this latest value to decide whether anyone is still
		 * asking for more work.
		 */
		kick = r->kick;

		pass.phys_yielded = false;

		/* runtime options: picked up by every context at the next wait */
		r->move_budget.max_ios		= c->opts.reconcile_move_ios_in_flight;
		r->move_budget.max_sectors	= c->opts.reconcile_move_bytes_in_flight >> 9;

		for (r->phase = 0; r->phase < ARRAY_SIZE(reconcile_phases); r->phase++) {
			reconcile_phase_start(c);

			/*
			 * Pending phases (the last entries in reconcile_phases[])
			 * only run when something queued a pending cookie. Break
			 * rather than continue: they're all at the end and skipping
			 * them is equivalent to ending the pass.
			 */
			if (reconcile_phase_is_pending(r->phase) &&
			    bkey_deleted(&pending_cookie.k))
				goto out;

			ret = do_reconcile_phase(&pass, kick);
			r->phase_exits[r->phase][ret
				? RECONCILE_PHASE_EXIT_error
				: pass.exit]++;
			if (ret)
				goto out;

			if (pass.exit == RECONCILE_PHASE_EXIT_yield) {
				pass.yielded = true;
				if (reconcile_phases[r->phase].type == RECONCILE_PHASE_phys)
					pass.phys_yielded = true;
			}

			work.nr = 0;

			if (kick != r->kick ||
			    test_bit(BCH_FS_going_ro, &c->flags) ||
			    bch2_move_ratelimit(ctxt))
				break;

			/* Drain pending moves before the next phase. */
			bch2_moving_ctxt_flush_all(ctxt);
		}

		/* Completed a clean pass through all phases — we're done. */
		if (r->phase == ARRAY_SIZE(reconcile_phases))
			break;
	}
out:
	if (!ret && !bkey_deleted(&pending_cookie.k))
		try(bch2_clear_reconcile_needs_scan(trans,
				pending_cookie.k.p, pending_cookie.v.cookie));

	bch2_move_stats_exit(&r->work_stats, c);

	if (!ret &&
	    !kthread_should_stop() &&
	    !atomic64_read(&r->work_stats.sectors_seen) &&
	    !sectors_scanned &&
	    !pass.yielded &&
	    kick == r->kick) {
		bch2_moving_ctxt_flush_all(ctxt);
		bch2_trans_unlock_long(trans);
		reconcile_wait(c, kick);
	}

	if (!bch2_err_matches(ret, EROFS))
		bch_err_fn(c, ret);
	return ret;
}

static int bch2_reconcile_thread(void *arg)
{
	struct bch_fs *c = arg;
	struct bch_fs_reconcile *r = &c->reconcile;

	set_freezable();

	/*
	 * Data move operations can't run until after check_snapshots has
	 * completed, and bch2_snapshot_is_ancestor() is available.
	 */
	kthread_wait_freezable(c->recovery.pass_done > BCH_RECOVERY_PASS_check_snapshots ||
			       kthread_should_stop());
	if (kthread_should_stop())
		return 0;

	struct moving_context ctxt __cleanup(bch2_moving_ctxt_exit);
	bch2_moving_ctxt_init(&ctxt, c, NULL, &r->work_stats,
			      writepoint_ptr(&c->allocator.reconcile_write_point),
			      true);
	bch2_moving_ctxt_set_budget(&ctxt, &r->move_budget);
	ctxt.metadata_throttle = true;

	while (!kthread_should_stop() && !do_reconcile(&ctxt))
		;

	return 0;
}

__cold void bch2_reconcile_status_to_text(struct printbuf *out, struct bch_fs *c)
{
	printbuf_tabstop_push(out, 24);
	printbuf_tabstop_push(out, 12);
	printbuf_tabstop_push(out, 12);

	struct bch_fs_reconcile *r = &c->reconcile;

	if (!r->running) {
		prt_printf(out, "waiting:\n");
		u64 now = atomic64_read(&c->io_clock[WRITE].now);

		prt_printf(out, "io wait duration:\t");
		bch2_prt_human_readable_s64(out, (r->wait_iotime_end - r->wait_iotime_start) << 9);
		prt_newline(out);

		prt_printf(out, "io wait remaining:\t");
		bch2_prt_human_readable_s64(out, (r->wait_iotime_end - now) << 9);
		prt_newline(out);

		prt_printf(out, "duration waited:\t");
		bch2_pr_time_units(out, ktime_get_real_ns() - r->wait_wallclock_start);
		prt_newline(out);
	} else {
		/*
		 * r->phase is bumped concurrently by do_reconcile(); after a
		 * clean pass through every phase it briefly equals
		 * ARRAY_SIZE(reconcile_phases) before r->running is cleared.
		 * Snapshot via READ_ONCE() and bounds-check.
		 */
		unsigned phase_idx = READ_ONCE(r->phase);
		struct bpos work_pos = r->work_pos.pos;
		barrier();

		if (phase_idx >= ARRAY_SIZE(reconcile_phases)) {
			prt_printf(out, "between phases\n");
		} else {
			struct reconcile_phase phase = reconcile_phases[phase_idx];

			if (phase.type == RECONCILE_PHASE_scan) {
				prt_printf(out, "scanning: ");
				struct reconcile_scan s = reconcile_scan_decode(c, work_pos.offset);
				reconcile_scan_to_text(out, c, s);

				if (s.type == RECONCILE_SCAN_fs ||
				    s.type == RECONCILE_SCAN_metadata) {
					prt_char(out, ' ');
					bch2_progress_to_text(out, &r->progress);
				}
				prt_newline(out);
			} else {
				prt_printf(out, "processing %s %s: ",
					   bch2_reconcile_work_ids[phase.priority],
					   bch2_reconcile_phase_types[phase.type]);

				if (phase.type == RECONCILE_PHASE_normal ||
				    phase.type == RECONCILE_PHASE_destage) {
					bch2_progress_to_text(out, &r->progress);
				} else {
					bch2_bpos_to_text(out, work_pos);
				}

				prt_newline(out);
			}
		}
	}

	prt_newline(out);
	prt_printf(out, "destage sweeps:\t%llu\n",		r->destage_sweeps);
	prt_printf(out, "destage time:\t");
	bch2_pr_time_units(out, r->destage_ns);
	prt_newline(out);
	prt_printf(out, "destage keys skipped:\t%llu\n",	r->destage_skipped);
	prt_printf(out, "destage keys attempted:\t%llu\n",	r->destage_attempted);
	prt_printf(out, "destage keys completed:\t%llu\n",	r->destage_completed);
	prt_printf(out, "destage keys deferred:\t%llu\n",	r->destage_deferred);
	prt_printf(out, "deferred, need copygc:\t%llu\n",	r->deferred);

	prt_newline(out);
	printbuf_tabstop_push(out, 12);
	printbuf_tabstop_push(out, 12);
	prt_printf(out, "moved since mount\treconcile\t\tcopygc\n");
	prt_printf(out, "device\tread\twritten\tread\twritten\n");
	for_each_member_device(c, ca) {
		prt_printf(out, "%u %s\t", ca->dev_idx, ca->name);
		prt_human_readable_u64(out, atomic64_read(&ca->reconcile_read_sectors) << 9);
		prt_tab(out);
		prt_human_readable_u64(out, atomic64_read(&ca->reconcile_write_sectors) << 9);
		prt_tab(out);
		prt_human_readable_u64(out, atomic64_read(&ca->copygc_read_sectors) << 9);
		prt_tab(out);
		prt_human_readable_u64(out, atomic64_read(&ca->copygc_write_sectors) << 9);
		prt_newline(out);
	}

	struct task_struct *t;
	scoped_guard(rcu) {
		t = rcu_dereference(c->reconcile.thread);
		if (t)
			get_task_struct(t);
	}

	prt_newline(out);

	if (t) {
		prt_str(out, "Reconcile thread backtrace:\n");
		guard(printbuf_indent)(out);
		bch2_prt_task_backtrace(out, t, 0, GFP_KERNEL);
		put_task_struct(t);
	} else {
		prt_str(out, "Reconcile thread not running\n");
	}
}

static __cold void reconcile_phase_name_to_text(struct printbuf *out, unsigned i)
{
	struct reconcile_phase p = reconcile_phases[i];

	prt_printf(out, "%s %s",
		   bch2_reconcile_work_ids[p.priority],
		   bch2_reconcile_phase_types[p.type]);
}

/*
 * Counters for tuning and diagnosing the reconcile scheduler; kept out of
 * reconcile_status, which sysfs truncates at a page and whose tail is the
 * thread backtrace.
 */
__cold void bch2_reconcile_stats_to_text(struct printbuf *out, struct bch_fs *c)
{
	struct bch_fs_reconcile *r = &c->reconcile;

	printbuf_tabstop_push(out, 24);
	for (unsigned i = 0; i < RECONCILE_PHASE_EXIT_NR; i++)
		printbuf_tabstop_push(out, 16);

	struct move_budget *b = &r->move_budget;
	prt_printf(out, "in flight, all contexts:\tios\tsectors\n");
	prt_printf(out, "reads\t%u\t%u\n", atomic_read(&b->read_ios), atomic_read(&b->read_sectors));
	prt_printf(out, "writes\t%u\t%u\n", atomic_read(&b->write_ios), atomic_read(&b->write_sectors));
	prt_printf(out, "limit\t%u\t%u\n\n", b->max_ios, b->max_sectors);

	prt_printf(out, "kicks since mount:\n");
	scoped_guard(printbuf_indent, out)
		for (unsigned i = 0; i < RECONCILE_KICK_NR; i++)
			prt_printf(out, "%s\t%llu\n",
				   bch2_reconcile_kick_reasons[i],
				   (u64) atomic64_read(&r->kicks[i]));

	prt_printf(out, "\nphase exits since mount:\n");
	prt_printf(out, "phase\t");
	for (unsigned i = 0; i < RECONCILE_PHASE_EXIT_NR; i++)
		prt_printf(out, "%s\t", bch2_reconcile_phase_exits[i]);
	prt_newline(out);

	for (unsigned i = 0; i < ARRAY_SIZE(reconcile_phases); i++) {
		reconcile_phase_name_to_text(out, i);
		prt_tab(out);
		for (unsigned j = 0; j < RECONCILE_PHASE_EXIT_NR; j++)
			prt_printf(out, "%llu\t", r->phase_exits[i][j]);
		prt_newline(out);
	}

	prt_printf(out, "\nlaps of resumable phases:\n");
	prt_printf(out, "phase\tcompleted\tin progress\tcursor\n");
	for (unsigned i = 0; i < ARRAY_SIZE(reconcile_phases); i++) {
		if (!reconcile_phase_resumable(i))
			continue;

		struct reconcile_lap lap = r->laps[i];

		reconcile_phase_name_to_text(out, i);
		prt_printf(out, "\t%llu\t%s\t", r->laps_completed[i],
			   !lap.active ? "no" : lap.wrapped ? "wrapped" : "yes");
		if (lap.active)
			bch2_bpos_to_text(out, lap.cursor);
		prt_newline(out);
	}

	prt_printf(out, "\nphys laps (rotational devices):\n");
	prt_printf(out, "device\thipri\tnormal\n");
	for_each_member_device(c, ca) {
		if (!ca->mi.rotational)
			continue;

		prt_printf(out, "%u %s", ca->dev_idx, ca->name);
		for (unsigned prio = 0; prio < 2; prio++) {
			struct reconcile_lap lap = r->phys_laps[ca->dev_idx * 2 + prio];

			prt_tab(out);
			if (!lap.active)
				prt_str(out, "idle");
			else
				prt_printf(out, "%s %llu", lap.wrapped ? "wrapped" : "at",
					   lap.cursor.offset);
		}
		prt_newline(out);
	}

	prt_printf(out, "\ncommitted since mount (moves completed and indexed):\n");
	prt_printf(out, "phase\tkeys\tdata\n");
	for (unsigned i = 0; i < ARRAY_SIZE(reconcile_phases); i++) {
		reconcile_phase_name_to_text(out, i);
		prt_printf(out, "\t%llu\t", (u64) atomic64_read(&r->phase_committed_keys[i]));
		prt_human_readable_u64(out, atomic64_read(&r->phase_committed_sectors[i]) << 9);
		prt_newline(out);
	}

	prt_printf(out, "\nper device:\tbuckets emptied\tcache evicted\tmove reads now\n");
	for_each_member_device(c, ca)
		prt_printf(out, "%u %s\t%llu\t%llu\t%u\n", ca->dev_idx, ca->name,
			   (u64) atomic64_read(&ca->buckets_emptied),
			   (u64) atomic64_read(&ca->buckets_evicted),
			   atomic_read(&ca->move_reads_in_flight));

	prt_printf(out, "\nmove outcomes since mount:\n");
	prt_printf(out, "outcome\treconcile\tcopygc\tparked pending\n");
	for (unsigned i = 0; i < MOVE_OUTCOME_NR; i++)
		prt_printf(out, "%s\t%llu\t%llu\t%llu\n",
			   bch2_move_outcomes[i],
			   (u64) atomic64_read(&r->move_outcomes[0][i]),
			   (u64) atomic64_read(&r->move_outcomes[1][i]),
			   (u64) atomic64_read(&r->pending_reasons[i]));
}

__cold void bch2_reconcile_scan_pending_to_text(struct printbuf *out, struct bch_fs *c)
{
	/*
	 * No multithreaded btree access until BCH_FS_may_go_rw and we're no
	 * longer modifying the journal keys gap buffer:
	 */
	if (!test_bit(BCH_FS_may_go_rw, &c->flags))
		return;

	CLASS(btree_trans, trans)(c);
	CLASS(btree_iter, iter)(trans, BTREE_ID_reconcile_scan, POS_MIN, 0);

	struct bkey_s_c k;
	lockrestart_do(trans, bkey_err(k = bch2_btree_iter_peek(&iter)));

	prt_printf(out, "%u\n", iter.pos.inode == 0);
}

void bch2_reconcile_stop(struct bch_fs *c)
{
	struct task_struct *p;

	p = rcu_dereference_protected(c->reconcile.thread, 1);
	c->reconcile.thread = NULL;

	if (p) {
		/* for sychronizing with bch2_reconcile_wakeup() */
		synchronize_rcu();

		kthread_stop(p);
		put_task_struct(p);
	}
}

int bch2_reconcile_start(struct bch_fs *c)
{
	if (c->reconcile.thread)
		return 0;

	if (c->opts.nochanges)
		return 0;

	struct task_struct *p =
		kthread_create(bch2_reconcile_thread, c, "bch-reconcile/%s", c->name);
	int ret = PTR_ERR_OR_ZERO(p);
	bch_err_msg(c, ret, "creating reconcile thread");
	if (ret)
		return ret;

	get_task_struct(p);
	rcu_assign_pointer(c->reconcile.thread, p);
	wake_up_process(p);
	return 0;
}

#ifdef CONFIG_POWER_SUPPLY
#include <linux/power_supply.h>

static int bch2_reconcile_power_notifier(struct notifier_block *nb,
					 unsigned long event, void *data)
{
	struct bch_fs *c = container_of(nb, struct bch_fs, reconcile.power_notifier);

	c->reconcile.on_battery = !power_supply_is_system_supplied();
	bch2_reconcile_wakeup(c, RECONCILE_KICK_power);
	return NOTIFY_OK;
}
#endif

static void reconcile_scan_in_flight_free(void *p, void *arg)
{
	WARN_ON_ONCE(1);
	kfree(p);
}

void bch2_fs_reconcile_exit(struct bch_fs *c)
{
	struct bch_fs_reconcile *r = &c->reconcile;

	if (r->scans_in_flight_init_done)
		rhashtable_free_and_destroy(&r->scans_in_flight,
					    reconcile_scan_in_flight_free, NULL);

	kvfree(r->phys_laps);
	r->phys_laps = NULL;

#ifdef CONFIG_POWER_SUPPLY
	power_supply_unreg_notifier(&r->power_notifier);
#endif
}

int bch2_fs_reconcile_init(struct bch_fs *c)
{
	BUILD_BUG_ON(ARRAY_SIZE(reconcile_phases) != RECONCILE_NR_PHASES);

	struct bch_fs_reconcile *r = &c->reconcile;

	r->phys_laps = kvcalloc(BCH_SB_MEMBERS_MAX * 2, sizeof(*r->phys_laps), GFP_KERNEL);
	if (!r->phys_laps)
		return bch_err_throw(c, ENOMEM_fs_other_alloc);

	init_waitqueue_head(&r->move_budget.wait);
	r->move_budget.max_ios		= c->opts.reconcile_move_ios_in_flight;
	r->move_budget.max_sectors	= c->opts.reconcile_move_bytes_in_flight >> 9;

	mutex_init(&r->scans_in_flight_lock);
	try(rhashtable_init(&r->scans_in_flight, &reconcile_scan_in_flight_params));
	r->scans_in_flight_init_done = true;

#ifdef CONFIG_POWER_SUPPLY
	r->power_notifier.notifier_call = bch2_reconcile_power_notifier;
	try(power_supply_reg_notifier(&r->power_notifier));

	r->on_battery = !power_supply_is_system_supplied();
#endif
	return 0;
}
