/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_MOVE_TYPES_H
#define _BCACHEFS_MOVE_TYPES_H

#include "btree/bbpos_types.h"
#include "bcachefs_ioctl.h"
#include "init/dev_types.h"
#include "util/darray.h"

struct bch_move_stats {
	char			name[32];
	bool			phys;
	enum bch_ioctl_data_event_ret	ret;

	union {
	struct {
		enum bch_data_type	data_type;
		struct bbpos		pos;
	};
	struct {
		unsigned		dev;
		u64			offset;
	};
	};

	atomic64_t		keys_moved;
	atomic64_t		keys_raced;
	atomic64_t		sectors_seen;
	atomic64_t		sectors_moved;
	atomic64_t		sectors_raced;
	atomic64_t		sectors_error_corrected;
	atomic64_t		sectors_error_uncorrected;
	struct bch_devs_mask	devs_error_uncorrected;
};

/*
 * In-flight move IO shared by several moving contexts - reconcile's thread
 * and its per-device phys workers - bounded in bch2_move_ratelimit() on top
 * of each context's own limits. Every context using it sleeps on @wait, so
 * any context's completion wakes waiters in all of them.
 */
struct move_budget {
	atomic_t		read_sectors;
	atomic_t		write_sectors;
	atomic_t		read_ios;
	atomic_t		write_ios;
	/* the limits' option values, read live; 0 means no limit */
	const u32		*max_bytes_opt;
	const u32		*max_ios_opt;
	wait_queue_head_t	wait;
};

struct move_bucket_key {
	struct bpos		bucket;
	unsigned		generation;
};

struct move_bucket {
	struct move_bucket	*next;
	struct rhash_head	hash;
	struct move_bucket_key	k;
	unsigned		sectors;
	atomic_t		count;
};

typedef struct {
	enum btree_id		btree_id;
	unsigned		bad_devs;
	__BKEY_PADDED(k, BKEY_EXTENT_VAL_U64s_MAX);
} scrub_journal_repair;

DEFINE_DARRAY(scrub_journal_repair);

#endif /* _BCACHEFS_MOVE_TYPES_H */
