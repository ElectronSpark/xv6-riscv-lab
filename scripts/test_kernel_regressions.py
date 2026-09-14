"""Regression checks for console failures observed in real QEMU runs."""

import unittest

from scripts.run_kernel_regressions import FAILURE


class ConsoleFailureTests(unittest.TestCase):
    def test_panics_are_detected_without_waiting_for_a_shell_timeout(self):
        for line in (
            "[500768929] clone: thread_group_alloc failed",
            "[500775485] [Core: 0] Received IPI_REASON_CRASH, crashing...",
            "KERNEL PANIC: bad state",
            "rustnettest: FAIL: interrupted read",
            "FAILED -- lost some free pages 66069 (out of 66160)",
        ):
            with self.subTest(line=line):
                self.assertIsNotNone(FAILURE.search(line))

    def test_expected_validation_messages_and_test_names_are_not_failures(self):
        for line in (
            "[7846602] vma_copyout: invalid vma for va 80200000",
            "test sbrkfail: OK",
            "test forkforkfork: OK",
            "rustnettest: ALL TESTS PASSED (64 UDP echoes)",
        ):
            with self.subTest(line=line):
                self.assertIsNone(FAILURE.search(line))


if __name__ == "__main__":
    unittest.main()
