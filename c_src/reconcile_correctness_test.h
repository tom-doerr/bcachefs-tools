/* SPDX-License-Identifier: GPL-2.0 */
/* Included by reconcile/work.c only in userspace, to exercise its real phase loop. */
int bch2_test_reconcile_pending(struct bch_fs *);

static int test_pending_blocked_prefix(struct reconcile_pass *p, struct bkey_s_c k)
{
	if (k.k->p.offset < 1 || k.k->p.offset > 17)
		return -EINVAL;
	*p->sectors_scanned |= BIT_ULL(k.k->p.offset);
	return k.k->p.offset <= 16 ? -BCH_ERR_data_update_fail_need_copygc : 0;
}

static int test_pending_resolved(struct reconcile_pass *p, struct bkey_s_c k)
{
	if (k.k->p.offset < 1 || k.k->p.offset > 17)
		return -EINVAL;
	*p->sectors_scanned |= BIT_ULL(k.k->p.offset);
	return 0;
}

static void test_pending_fill(darray_reconcile_work *work)
{
	work->nr = 17;
	for (unsigned i = 0; i < 17; i++) {
		bkey_init(&work->data[i].k);
		work->data[i].k.type = KEY_TYPE_set;
		work->data[i].k.p = POS(0, 17 - i);
	}
}

static int test_pending_cookie_read(struct btree_trans *trans, struct bkey_i_cookie *cookie)
{
	CLASS(btree_iter, iter)(trans, BTREE_ID_reconcile_scan,
				POS(0, RECONCILE_SCAN_COOKIE_pending), 0);
	struct bkey_s_c k = bkey_try(bch2_btree_iter_peek_slot(&iter));
	if (k.k->type != KEY_TYPE_cookie)
		return -ENOENT;
	bkey_reassemble(&cookie->k_i, k);
	return 0;
}

int bch2_test_reconcile_pending(struct bch_fs *c)
{
	/* The fixture starts with reconcile disabled: drive the loop ourselves. */
	bch2_reconcile_stop(c);
	struct moving_context ctxt __cleanup(bch2_moving_ctxt_exit);
	bch2_moving_ctxt_init(&ctxt, c, NULL, &c->reconcile.work_stats,
			      writepoint_ptr(&c->allocator.reconcile_write_point), true);
	struct btree_trans *trans = ctxt.trans;
	CLASS(darray_reconcile_work, work)();
	try(darray_make_room(&work, RECONCILE_WORK_BUF_NR));
	CLASS(darray_stripe_retry, retry)();
	CLASS(per_snapshot_io_opts, opts)(c);
	struct wb_maybe_flush last_flushed __cleanup(wb_maybe_flush_exit);
	wb_maybe_flush_init(&last_flushed);
	struct bkey_i_cookie cookie;
	bkey_init(&cookie.k);
	u64 visited = 0;
	u32 run_count = READ_ONCE(c->copygc.run_count);
	struct reconcile_pass pass = {
		.ctxt = &ctxt, .snapshot_io_opts = &opts, .work = &work,
		.last_flushed = &last_flushed, .stripe_retry = &retry,
		.pending_cookie = &cookie, .sectors_scanned = &visited,
		.copygc_run_count = &run_count,
	};
	try(bch2_set_reconcile_needs_scan(c,
		(struct reconcile_scan) { .type = RECONCILE_SCAN_pending }, false));
	try(lockrestart_do(trans, test_pending_cookie_read(trans, &cookie)));
	bch2_trans_unlock(trans);
	try(do_reconcile_scan_key(&pass, bkey_i_to_s_c(&cookie.k_i)));
	/* Observing the request must leave its durable cookie intact. */
	try(lockrestart_do(trans, test_pending_cookie_read(trans, &cookie)));
	bch2_trans_unlock(trans);

	WRITE_ONCE(c->opts.reconcile_enabled, true);
	c->reconcile.phase = ARRAY_SIZE(reconcile_phases) - 1;
	reconcile_phase_start(c);
	test_pending_fill(&work);
	visited = 0;
	u64 start = ktime_get_ns();
	int ret = do_reconcile_phase_iter(&pass, c->reconcile.kick, test_pending_blocked_prefix);
	if (!ret && (visited != (BIT_ULL(18) - 2) || !pass.pending_deferred ||
		     !pass.phase_exhausted || ktime_get_ns() - start > 5 * NSEC_PER_SEC))
		ret = -EINVAL;
	bch2_trans_unlock(trans);
	if (!ret) {
		/* Interruption is distinct from reaching the end of the phase. */
		reconcile_phase_start(c);
		test_pending_fill(&work);
		pass.phase_exhausted = false;
		visited = 0;
		ret = do_reconcile_phase_iter(&pass, c->reconcile.kick - 1, test_pending_resolved);
		if (!ret && (visited || pass.phase_exhausted))
			ret = -EINVAL;
	}
	if (!ret) {
		pass.pending_deferred = false;
		ret = do_reconcile_phase_iter(&pass, c->reconcile.kick, test_pending_resolved);
		if (!ret && (visited != (BIT_ULL(18) - 2) || pass.pending_deferred || !pass.phase_exhausted))
			ret = -EINVAL;
	}
	bch2_trans_unlock(trans);
	WRITE_ONCE(c->opts.reconcile_enabled, false);
	if (!ret) {
		/* A newer request must survive acknowledgement of the old version. */
		ret = bch2_set_reconcile_needs_scan(c,
			(struct reconcile_scan) { .type = RECONCILE_SCAN_pending }, false) ?:
			bch2_clear_reconcile_needs_scan(trans, cookie.k.p, le64_to_cpu(cookie.v.cookie)) ?:
			lockrestart_do(trans, test_pending_cookie_read(trans, &cookie));
	}
	bch2_trans_unlock(trans);
	int cleanup = bch2_clear_reconcile_needs_scan(trans, cookie.k.p, le64_to_cpu(cookie.v.cookie));
	fprintf(stderr, "storage reliability: pending prefix, disabled copygc, interruption and newer cookie: %d\n",
		ret ?: cleanup);
	return ret ?: cleanup;
}
