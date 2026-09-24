/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_REBALANCE_TYPES_H
#define _BCACHEFS_REBALANCE_TYPES_H

#include "btree/bbpos_types.h"
#include "data/move_types.h"
#include "init/progress.h"

#include <linux/mutex.h>
#include <linux/rhashtable-types.h>

/* Who woke the reconcile thread: a kick restarts the pass from phase 0 */
#define RECONCILE_KICK_REASONS()	\
	x(sysfs)			\
	x(inode_opts)			\
	x(remount)			\
	x(device_online)		\
	x(recovery)			\
	x(opt_change)			\
	x(read_only)			\
	x(scan_cookie)			\
	x(opt_change_settled)		\
	x(power)

enum reconcile_kick_reason {
#define x(n)	RECONCILE_KICK_##n,
	RECONCILE_KICK_REASONS()
#undef x
	RECONCILE_KICK_NR,
};

/* Why a phase returned to do_reconcile() */
#define RECONCILE_PHASE_EXITS()		\
	x(exhausted)			\
	x(kick)				\
	x(yield)			\
	x(deferred_limit)		\
	x(skipped)			\
	x(stopped)			\
	x(error)

enum reconcile_phase_exit {
#define x(n)	RECONCILE_PHASE_EXIT_##n,
	RECONCILE_PHASE_EXITS()
#undef x
	RECONCILE_PHASE_EXIT_NR,
};

/* ARRAY_SIZE(reconcile_phases), checked in work.c */
#define RECONCILE_NR_PHASES		10

/* How a reconcile or copygc data update ended, see bch2_move_outcome(): */
#define MOVE_OUTCOMES()			\
	x(ok)				\
	x(no_io)			\
	x(in_flight)			\
	x(need_copygc)			\
	x(would_block)			\
	x(blocked)			\
	x(no_rw_devs)			\
	x(insufficient_devices)		\
	x(enospc)			\
	x(no_snapshot)			\
	x(erofs)			\
	x(other)

enum move_outcome {
#define x(n)	MOVE_OUTCOME_##n,
	MOVE_OUTCOMES()
#undef x
	MOVE_OUTCOME_NR,
};

/*
 * A lap is one pass over a long phase's keyspace, from its start to its end;
 * interruptions (kicks, time slices, deferral limits) save the cursor, and
 * the phase resumes there instead of starting over.
 */
struct reconcile_lap {
	struct bpos			cursor;
	bool				active;
};

struct bch_fs_reconcile {
	struct task_struct __rcu	*thread;
	u32				kick;
	atomic64_t			kicks[RECONCILE_KICK_NR];

	bool				running;
	u64				wait_iotime_start;
	u64				wait_iotime_end;
	u64				wait_wallclock_start;

	unsigned			phase;
	struct bbpos			work_pos;
	struct bch_move_stats		work_stats;
	struct progress_indicator	progress;

	struct bbpos			scan_start;
	struct bbpos			scan_end;
	struct bch_move_stats		scan_stats;

	/*
	 * Since mount, written only by the reconcile thread: the destage
	 * prepass, and need_copygc deferrals in all keyed phases:
	 */
	u64				destage_sweeps;
	u64				destage_ns;
	u64				destage_skipped;
	u64				destage_attempted;
	u64				destage_completed;
	u64				destage_deferred;
	u64				deferred;

	/* Destage keys attempted in the current destage lap */
	u64				destage_lap_attempted;
	/* After a lap that found nothing: skip destage until (ktime ns) */
	u64				destage_idle_until;

	/* In-flight IO across the reconcile thread and its phys workers */
	struct move_budget		move_budget;

	/*
	 * Per-device laps of the phys phases, [0] hipri, [1] normal, indexed
	 * by dev * 2 + priority; each written only by that device's worker:
	 */
	struct reconcile_lap		*phys_laps;

	/* Written only by the reconcile thread: */
	struct reconcile_lap		laps[RECONCILE_NR_PHASES];
	u64				laps_completed[RECONCILE_NR_PHASES];
	u64				phase_exits[RECONCILE_NR_PHASES][RECONCILE_PHASE_EXIT_NR];

	/*
	 * Since mount, from move completions and phys workers: work actually
	 * committed per phase (not just started), how moves ended ([0]
	 * reconcile, [1] copygc), and why work was parked as pending:
	 */
	atomic64_t			phase_committed_keys[RECONCILE_NR_PHASES];
	atomic64_t			phase_committed_sectors[RECONCILE_NR_PHASES];
	atomic64_t			move_outcomes[2][MOVE_OUTCOME_NR];
	atomic64_t			pending_reasons[MOVE_OUTCOME_NR];

	/* In-flight opt changes - see bch2_set_reconcile_needs_scan_pre/post() */
	struct rhashtable		scans_in_flight;
	bool				scans_in_flight_init_done;
	struct mutex			scans_in_flight_lock;

	bool				on_battery;
#ifdef CONFIG_POWER_SUPPLY
	struct notifier_block		power_notifier;
#endif
};

#endif /* _BCACHEFS_REBALANCE_TYPES_H */
