# Filesystem reliability fixes and validation

This branch addresses bcachefs metadata durability, error reporting, and
maintenance progress. It implements corrections for findings F1–F26 and S1–S6
from the correctness review of `abc78b6f520164b9b1cb9011506c733114e5ed3c`.
This is storage reliability maintenance, not a cybersecurity assessment.

Work and tests used an isolated checkout and freshly created local regular-file
images. No NAS access, production mount, kernel installation, recovery-data
change, or shared-VM operation was involved. These findings do not establish
the cause of the historical NAS incident.

The input report, `bcachefs-correctness-review-20260927.md`, was preserved with
SHA256 `25a3d8f571fd7a9cda0000ef4b0c70174fca5c44a744f6ddb1d2706fc35ca8fb`.
The branch is `fix/correctness-review-20260927` in `tom-doerr/bcachefs-tools`.

## Finding coverage

Every row received source/caller review and compilation of the affected C code.
The runtime column identifies additional coverage; “not exercised” means the
specific failure case has not been reproduced by these tests.

| Finding | Correction and commit | Runtime coverage |
| --- | --- | --- |
| F1 | Write-lock cached keys when advancing their journal pin; `eb559bc63` | Real journal callback forces restart after dropped locks; old intent-only control relocks successfully |
| F2 | Acknowledge pending cookies after completed sweeps; preserve deferred/interrupted work and reach beyond blocked prefixes; `873fd840b` | Real phase loop visits 17 prefetched test entries after 16 deferrals; durable cookie and newer-version checks |
| F3 | Pass the computed metadata copygc reserve flags; `f0c84a88b` | Low-space metadata copygc not exercised |
| F4 | Propagate bucket-check failures and advance the checked cursor only on success; `f0c84a88b` | Terminal bucket-check failure not exercised |
| F5 | Retain dirty cache identity until writeback; release node locks before waiting and reselect a clean candidate; `96209a871` | Real dirty leaf is written, evicted, reread, and fsck-checked; separately redirtied/in-flight case not exercised |
| F6 | Fail a journal commit when any required member preflush fails; abort replica submission and release reserved references; `bbcf7dce0`, `ab515fc9a` | One required flush fails on a member with no journal replica; journal caller gets an error and filesystem enters emergency read-only handling |
| F7 | Serialize the replacement journal bucket array; `8d388d07e` | Delete a middle bucket and reread the persisted superblock; original list restored before subsequent tests |
| F8 | Return nocow flush errors and restore failed device flush bits; `e7ccb9673` | Kernel compilation only; VFS fsync failure not exercised |
| F9 | Separate interrupted scans from real failures, report exited workers, and reap/restart them; `27d4afde9`, `55b7bf6df`, `a974fc125` | Full worker stop/restart scenario not exercised |
| F10 | Carry physical worker errors and deferral state to the parent; `873fd840b` | Per-device worker failure not exercised |
| F11 | Bound copygc waits and observe disabled/stopping states; `873fd840b` | Pending phase returns promptly with copygc disabled; loaded-kernel timeout/wakeup behavior not exercised |
| F12 | Bound repair evacuation by live data blocks; `57e86b206` | Below-parity device case not exercised |
| F13 | Handle deleted stripes without dereferencing a missing new value during GC; `05cea2421` | GC deletion path not exercised |
| F14 | Hold the stripe-head lock while formatting its current stripe; `691703315` | Concurrent diagnostic/teardown case not exercised |
| F15 | Permit stripe copygc to move a full live data bucket while retaining type/liveness checks; `f6ed6e996` | Full-bucket stripe evacuation not exercised |
| F16 | Remember deletion deferred by an open handle; retry on close and on transient worker errors; `8cb521c48` | Deletion retry scheduling not directly exercised |
| F17 | Serialize global/per-stripe sequence publication with the flush snapshot; `aece49073` | Concurrent publication/flush schedule not exercised |
| F18 | Support clearing a stripe reconcile marker and commit it on completed/no-op repair; `6d0de6915` | Healthy-stripe setter clears a copied marker; durable repair-handler commit not directly exercised |
| F19 | Preserve EC feasibility error pointers; `4aba9c920` | Error-pointer cases not exercised |
| F20 | Account for internally consumed transaction restarts in journal scrub; `6da4df500` | Failed-read nested restart not exercised |
| F21 | Keep scrub iterators off the recovery-only mutable list while keys are frozen; `14ef1be3b` | Concurrent journal scrub retry not exercised |
| F22 | Unroot and write-lock nodes before topology repair changes cache identity; `85d2a2ef1`, `746c74538` | Corrupt-root repair cases not exercised |
| F23 | Give generation GC a per-key disk reservation; `2a6a704cd` | Reservation-charging GC case not exercised |
| F24 | Elevate extents commit reserve access before journal reservation; `d445cf6ff` | Open-bucket/journal pressure stall not exercised |
| F25 | Consult existing alloc-key cache entries without filling it during scrub; `e3429d3fd` | Device-scale memory-pressure case not exercised |
| F26 | Skip offline replica pointers during journal scrub; `4650311f3` | Degraded unclean recovery case not exercised |
| S1 | Wait for the oldest outstanding move before stripe retry; `a76547883` | Out-of-order I/O completion not exercised |
| S2 | Search both pinnedness lists when reclaiming matching node lock types; `96209a871` | Self-reclaim under actual memory pressure not exercised |
| S3 | Propagate journal-superblock conversion failure; `8d388d07e` | Allocation failure during conversion not exercised |
| S4 | Attribute failed background reads to direct extent owners; `bb45baabe` | Failed background read not exercised; frozen journal scrub deliberately does not commit attribution |
| S5 | Publish a newly allocated stripe position only after successful memory preparation; `691703315` | Latent in-GC preparation failure not exercised |
| S6 | Preserve terminal checksum classification and retry/scrub bookkeeping failures; `bfa24c051`, `a76547883`, `bb45baabe`, `873fd840b` | Specific checksum/allocation error cases not exercised |

Later-upstream corrections already present in the local source history were
ported with their relevant caller contracts: F9 (`a6cfc02c`, `7a1b3f75`,
`42cb08fe`), F19 (`aced5322`), F20 (`289daef1`), F21 (`d5bdd453`),
F22 (`e69ef85d`, `b4dfb540`), F23 (`2379d012`), F24 (`6c47735d`),
F25 (`b5735c4b`), F26 (`e9b35d2a`), and checksum classification
(`7e984a42`). Background damage attribution adapts `2077f8ee` while excluding
the frozen, nonrepairing journal-scrub path. This does not claim an audit of
the current upstream head.

The report's rejected candidates remain rejected. No additional fix was made
for ordinary iterator-owned mover key headers, handle ABA, pin FIFO resizing,
write-buffer dedup ordering, btree object lifetime, or intentional preferred-read
fallback. The post-baseline reuse-abort finding depends on later code and was
not imported.

## Recorded validation

Production and test code at `7b9f624da6a4ceba25c9d9539b9c9d19b7c8d698`:

- Complete userspace C/Rust release build passed on ARM64.
- Ordinary binary unit tests: **40 passed**, three storage-image tests ignored
  by default and run explicitly below.
- Explicit storage-image suite: **3 passed**. Each test formats three fresh
  sparse 1 GiB regular files, imports 32 MiB of EC data, and runs offline
  `fsck -K -f -n` before and after its C regression.
- Original EC lifetime regression still passes all six old/fixed cases
  (uncached leaf, cache overlay, explicit cached iterator). Original helper:
  relock succeeds; fixed helper: transaction restart. See
  [the EC regression explanation](ec-stripe-lifetime-regression.md).
- Dirty-cache test: committed leaf write position advanced **104 → 112
  sectors** before eviction; the uncached reread matched its saved key.
  Fixture root growth uses the normal btree update API, and selection uses
  temporary noevict flags instead of exhausting system memory.
- Required preflush test: exactly **one** simulated member flush failure;
  journal caller and journal state both returned **-2509**. The failing device
  did not gain a journal replica. Offline fsck after the expected unclean
  shutdown passed.
- All **20 changed kernel C files** compiled against ARM64
  `6.17.0-1031-nvidia` headers with `BCACHEFS_DEBUG=1`, `BCACHEFS_WERROR=1`,
  and `BCACHEFS_RUST=0`. The compiler-name warning identifies the same GCC
  13.3.0 package under native and cross-prefixed names. This is a focused C
  compilation, not a complete linked kernel-module build or kernel runtime test.
- `git diff --check` passed. The regular library has an unresolved `fdatasync`
  reference, not the test override. The override is built as a separate shared
  library, explicitly excluded from `libbcachefs.a`.

Runtime coverage is deliberately narrower than the source corrections. In
particular, this does not validate real-device power-loss behavior, the nocow
VFS fsync path, every repair state, or large-device reconcile/copygc workloads.
No production deployment is implied by these results.

## Reproducing the userspace regressions

With the normal userspace build dependencies configured, from this checkout:

```sh
RUSTFLAGS='-C default-linker-libraries' make -j4 bcachefs
RUSTFLAGS='-C default-linker-libraries' cargo test --release --bin bcachefs \
  -- --test-threads=1

storage_test_dir=$(mktemp -d)
cc -shared -fPIC -O2 -Wall -Wextra tests/storage_flush_error.c \
  -o "$storage_test_dir/storage_flush_error.so"

RUSTFLAGS='-C default-linker-libraries' \
LD_PRELOAD="$storage_test_dir/storage_flush_error.so" \
BCACHEFS_TEST_TMPDIR="$storage_test_dir" \
cargo test --release --bin bcachefs \
  -- --ignored --nocapture --test-threads=1
```

`BCACHEFS_TEST_BIN` optionally selects the formatter/fsck executable. Rebuild
both `libbcachefs.a` and the binary after C changes: invoking Cargo alone does
not rebuild the C library. No root privileges, block devices, mounts, or NAS
connection are needed. Userspace filesystem startup does need permission to
create its local status socket.

The standalone flush library is inactive until the C regression selects a file
descriptor belonging to its disposable image. It returns one `EIO` for that
descriptor's next flush and then disables itself. It is not installed or linked
into the normal binary. Expected I/O-error and emergency-read-only messages
are part of that test's success case.

The pending-work test uses controlled handlers and an in-memory prefetch buffer
to drive the real phase loop, plus real on-disk cookie operations. It does not
simulate a full device-placement workload. The pin test uses real journal pins
and real transaction locks; its controlled old implementation demonstrates why
the write-lock sequence change matters.

## Preserved evidence

Local evidence lives under `/home/tom/bcachefs-correctness-tests-20260927`:
the original report/hash, build environment wrapper, build logs, intermediate
failed-run logs and images, final test logs, and all final disposable images.
The final logs are `build-userspace-final.log`, `unit-tests-final.log`,
`regression-final.log`, `kernel-c-build-final.log`, and
`flush-shim-link-check-final.txt`. Each final fixture includes format and
before/after fsck output. Earlier NAS incident evidence was not modified.

The failed runs are retained to distinguish test-setup errors from code errors.
In particular, the required-flush test caught a missing return after
`continue_at()` in the first F6 implementation; `ab515fc9a` corrects it, and
the final recorded run includes that correction.
