# QEMU-driven ctest entries for the in-kernel Rust runtime test suites
# (docs/rustify/test_port_plan.md Phase 3/4).
#
# Each suite below gets its own ctest entry that:
#   * builds the *whole kernel* in an ISOLATED build directory under
#     test/build_qemu_<suite>/ (never the main build/ -- a feature-gated
#     rebuild must not perturb that tree's CMakeCache.txt / cargo target
#     dir, and each suite needs a different cargo feature enabled, which
#     forces a different xv6_rust rebuild -- see kernel/CMakeLists.txt's
#     RUST_TEST_FEATURES stamp-file comment for why that has to be a fresh
#     `cargo build --features` invocation per feature combination anyway),
#   * boots that kernel in QEMU under a timeout,
#   * greps the captured console log for the suite's own pass marker and
#     absence of its own fail marker, plus the universal boot gate
#     (`init: starting sh` exactly once).
#
# All the real logic lives in scripts/run_qemu_test.sh (also sets the
# MANDATORY TOOLPREFIX/LAB build env itself -- see RUST_REWRITE.md's
# "Working notes" for why every cmake-invoking script must do this itself
# rather than trust an inherited shell).
#
# Serialization: only one QEMU instance may run at a time on this machine
# (shared, limited resource -- and multiple kernels booting concurrently
# would also contend for the same isolated-build-dir disk I/O). Every test
# below sets `RESOURCE_LOCK qemu_vm` (ctest's resource-lock mechanism) so
# `ctest -j<N>` never overlaps two of them, regardless of how the caller
# invokes ctest; RUN_SERIAL is also set as a belt-and-suspenders backstop.

set(QEMU_TEST_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/run_qemu_test.sh)

function(add_qemu_test)
    set(options "")
    set(oneValueArgs NAME BUILD_SUBDIR TIMEOUT MUST_MATCH MUST_NOT_MATCH)
    set(multiValueArgs ENV)
    cmake_parse_arguments(AQT "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_test(
        NAME ${AQT_NAME}
        COMMAND ${QEMU_TEST_SCRIPT}
                ${CMAKE_CURRENT_SOURCE_DIR}/build_qemu_${AQT_BUILD_SUBDIR}
                ${AQT_TIMEOUT}
                "${AQT_MUST_MATCH}"
                "${AQT_MUST_NOT_MATCH}"
                --
                ${AQT_ENV}
    )
    set_tests_properties(${AQT_NAME} PROPERTIES
        LABELS "qemu;slow"
        RESOURCE_LOCK qemu_vm
        RUN_SERIAL TRUE
        # Generous ctest-level timeout on top of the script's own internal
        # `timeout` call -- catches a hang in the configure/build steps
        # themselves, which the internal `timeout` (scoped to just the
        # qemu invocation) does not cover.
        TIMEOUT ${AQT_TIMEOUT}
    )
    math(EXPR CTEST_TIMEOUT "${AQT_TIMEOUT} + 120")
    set_tests_properties(${AQT_NAME} PROPERTIES TIMEOUT ${CTEST_TIMEOUT})
endfunction()

# ------------------------------------------------------------------------
# rwsem / semaphore: existing in-kernel suites (lock/{rwsem,semaphore}_test.rs),
# gated by the pre-existing RWLOCK_TEST/SEMAPHORE_TEST env vars (Wave 27).
# Both print per-case "... OK"/"FAIL" lines and a final "tests finished"
# line -- there's no aggregate "N/N" summary to grep in these two (that
# convention was only added for the Phase 4 suites below), so the marker
# pair here is "reached the finished line" / "no case in this suite failed".
# ------------------------------------------------------------------------
add_qemu_test(
    NAME qemu_rwsem
    BUILD_SUBDIR rwsem
    TIMEOUT 90
    MUST_MATCH "\\[rwsem\\] tests finished"
    MUST_NOT_MATCH "\\[rwsem\\]\\[T[0-9]+\\].*FAIL"
    ENV RWLOCK_TEST=1
)

add_qemu_test(
    NAME qemu_semaphore
    BUILD_SUBDIR semaphore
    TIMEOUT 90
    MUST_MATCH "\\[sem\\] tests finished"
    MUST_NOT_MATCH "\\[sem\\]\\[T[0-9]+\\].*FAIL"
    ENV SEMAPHORE_TEST=1
)

# ------------------------------------------------------------------------
# pcache / workqueue: new Phase 4 suites (kernel/mm/pcache_test.rs,
# kernel/proc/workqueue_test.rs), gated by the new PCACHE_TEST/
# WORKQUEUE_TEST env vars this same task added to kernel/CMakeLists.txt.
# Both print a canonical "SUITE TESTS: N/N PASSED" (or "FAILED") summary
# line.
# ------------------------------------------------------------------------
add_qemu_test(
    NAME qemu_pcache
    BUILD_SUBDIR pcache
    TIMEOUT 150
    MUST_MATCH "PCACHE TESTS: [0-9]+/[0-9]+ PASSED"
    MUST_NOT_MATCH "PCACHE TESTS: [0-9]+/[0-9]+ FAILED"
    ENV PCACHE_TEST=1
)

add_qemu_test(
    NAME qemu_workqueue
    BUILD_SUBDIR workqueue
    TIMEOUT 90
    MUST_MATCH "WORKQUEUE TESTS: [0-9]+/[0-9]+ PASSED"
    MUST_NOT_MATCH "WORKQUEUE TESTS: [0-9]+/[0-9]+ FAILED"
    ENV WORKQUEUE_TEST=1
)
