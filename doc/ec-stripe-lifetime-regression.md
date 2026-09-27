# EC stripe opening and optimistic transaction relocking

An existing stripe can be opened after another transaction has decided to
delete it, but before that transaction commits. The deletion trigger checks
`bch2_stripe_is_open()` with the key intent-locked. Commit can subsequently drop
its locks, for example while waiting for a journal reservation. Meanwhile,
`get_old_stripe()` or `bch2_stripe_repair()` can read the still-committed stripe
and publish an open handle.

Previously publication changed only the open-stripe hash table. An intent
lock excludes a deletion holding its locks, but does not change the SIX lock
sequence. The deletion's optimistic relock could therefore succeed despite
its stale closed-stripe decision. The commit retry is below the transactional
trigger pass, so that pass is not repeated on a successful relock.

`bch2_stripe_handle_tryget_existing()` publishes an existing-stripe handle
under the write lock of the path supplying the stripe key. Write unlock
advances the normal SIX sequence, making the stale transaction restart and
recheck the open handle. It uses the key-cache path when a normal iterator
read through the cache, the explicit cached path when applicable, and the
leaf path for an uncached key. The write lock is acquired before publication,
so a lock error or restart does not leave an open handle behind. Both reuse
and repair call this helper. Allocation of an unused stripe slot keeps the
hash-only helper.

## Regression

After building the userspace binary and C library:

```sh
make -j4 bcachefs
RUSTFLAGS='-C default-linker-libraries' cargo test --release --bin bcachefs \
  ec_lifetime -- --ignored --nocapture --test-threads=1
```

The ignored test creates three fresh sparse 1 GiB regular files in a private
temporary directory. Normal `format --source` imports 32 MiB with 2+1 EC; it
does not manufacture persistent metadata. `BCACHEFS_TEST_BIN` can select the
binary and `BCACHEFS_TEST_TMPDIR` can select the artifact parent. Logs and
images are retained at the printed path.

The C test opens the completed fixture read-only and interleaves two real
transactions on an initialized userspace worker. For cached and cache-overlay
opening paths it stages a live-to-empty stripe transition through the real
stripe trigger, using the explicit cached iterator employed by extent
accounting. It verifies that both cached and leaf deletion updates retain
their optimistic-lock dependencies, then unlocks the deletion transaction.
A second transaction opens the still-committed stripe before the first
attempts to relock.

For each opening path, the original hash-only helper allows relock, whereas
the new helper forces a transaction restart. A retried trigger leaves an open
stripe alive; closing the handle permits deletion again. A separate uncached
guard-reader case verifies the leaf-path fallback. All three cases also check
that a second claimant receives busy without acquiring a handle.

Every staged update is discarded. The committed stripe is compared byte for
byte with its original value, and read-only offline fsck must pass both before
and after the test. This exercises real trigger staging, handle publication,
and relocking. It does not commit an extent unlink or demonstrate the entire
subsequent bucket-reclamation and EC I/O sequence.

## Validation recorded on ARM64

On the patch based on `de83fc674bcc6adf8221f9138e62c09b035283af`, the complete
userspace build passed. The regression passed all six old/fixed path cases:

| Opening path | Original helper relock | Fixed helper relock |
| --- | --- | --- |
| Uncached leaf | success | transaction restart |
| Normal iterator with cached key | success | transaction restart |
| Explicit cached iterator | success | transaction restart |

The changed EC C objects also compiled with debug checks and warnings treated
as errors against ARM64 `6.17.0-1031-nvidia` headers, with Rust disabled for
that focused compilation. This is not a complete kernel-module build, kernel
runtime test, production-kernel certification, or deployment. The test proves
the concurrency defect and the helper's effect; attributing a past incident
to this exact thread schedule still requires inference from its evidence.
