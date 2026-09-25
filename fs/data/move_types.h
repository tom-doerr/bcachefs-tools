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
 * Moves in flight across several moving contexts - reconcile's thread and its
 * per-device phys workers - on top of each context's own limits. Every move
 * holds a reservation (one move, its sectors) from admission until it is
 * completely done; see bch2_move_budget_get(). Every context using it sleeps
 * on @wait, so any context's completion wakes waiters in all of them.
 */
struct move_budget {
	atomic_t		moves;
	atomic_t		sectors;
	/* waiters for an item bigger than the byte limit: everyone else holds back */
	atomic_t		draining;
	/* the limits' option values, read live; 0 means no limit */
	const u32		*max_bytes_opt;
	const u32		*max_ios_opt;
	/* since mount: highest moves/sectors held, largest reservation, waits */
	atomic_t		peak_moves;
	atomic_t		peak_sectors;
	atomic_t		max_item_sectors;
	atomic64_t		waits;
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
