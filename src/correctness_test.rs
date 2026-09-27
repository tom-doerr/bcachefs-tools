//! Storage reliability regressions using disposable filesystem images.
use std::ffi::{c_char, c_int};

unsafe extern "C" {
    fn rust_test_storage_correctness(paths: *const *const c_char, nr: u32) -> c_int;
}

#[test]
#[ignore = "creates three sparse 1 GiB regular files and imports EC data"]
fn storage_correctness_journal_and_stripes() {
    crate::ec_lifetime_test::run_disposable_fixture("storage-correctness", rust_test_storage_correctness);
}
