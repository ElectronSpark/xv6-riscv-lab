# CMake generated Testfile for 
# Source directory: /home/es/xv6/xv6-riscv/test
# Build directory: /home/es/xv6/xv6-riscv/test/build_host
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(ut_list "/home/es/xv6/xv6-riscv/test/build_host/ut_list")
set_tests_properties(ut_list PROPERTIES  WORKING_DIRECTORY "/home/es/xv6/xv6-riscv/test/build_host" _BACKTRACE_TRIPLES "/home/es/xv6/xv6-riscv/test/CMakeLists.txt;319;add_test;/home/es/xv6/xv6-riscv/test/CMakeLists.txt;0;")
add_test(ut_bits "/home/es/xv6/xv6-riscv/test/build_host/ut_bits")
set_tests_properties(ut_bits PROPERTIES  WORKING_DIRECTORY "/home/es/xv6/xv6-riscv/test/build_host" _BACKTRACE_TRIPLES "/home/es/xv6/xv6-riscv/test/CMakeLists.txt;327;add_test;/home/es/xv6/xv6-riscv/test/CMakeLists.txt;0;")
add_test(rust_host_unit_tests "/usr/bin/cargo" "test" "--target" "x86_64-unknown-linux-gnu")
set_tests_properties(rust_host_unit_tests PROPERTIES  ENVIRONMENT "LAB=fs" WORKING_DIRECTORY "/home/es/xv6/xv6-riscv/test/../kernel" _BACKTRACE_TRIPLES "/home/es/xv6/xv6-riscv/test/CMakeLists.txt;368;add_test;/home/es/xv6/xv6-riscv/test/CMakeLists.txt;0;")
add_test(qemu_rwsem "/home/es/xv6/xv6-riscv/test/../scripts/run_qemu_test.sh" "/home/es/xv6/xv6-riscv/test/build_qemu_rwsem" "90" "\\[rwsem\\] tests finished" "\\[rwsem\\]\\[T[0-9]+\\].*FAIL" "--" "RWLOCK_TEST=1")
set_tests_properties(qemu_rwsem PROPERTIES  LABELS "qemu;slow" RESOURCE_LOCK "qemu_vm" RUN_SERIAL "TRUE" TIMEOUT "210" _BACKTRACE_TRIPLES "/home/es/xv6/xv6-riscv/test/cmake/QemuTests.cmake;37;add_test;/home/es/xv6/xv6-riscv/test/cmake/QemuTests.cmake;69;add_qemu_test;/home/es/xv6/xv6-riscv/test/cmake/QemuTests.cmake;0;;/home/es/xv6/xv6-riscv/test/CMakeLists.txt;387;include;/home/es/xv6/xv6-riscv/test/CMakeLists.txt;0;")
add_test(qemu_semaphore "/home/es/xv6/xv6-riscv/test/../scripts/run_qemu_test.sh" "/home/es/xv6/xv6-riscv/test/build_qemu_semaphore" "90" "\\[sem\\] tests finished" "\\[sem\\]\\[T[0-9]+\\].*FAIL" "--" "SEMAPHORE_TEST=1")
set_tests_properties(qemu_semaphore PROPERTIES  LABELS "qemu;slow" RESOURCE_LOCK "qemu_vm" RUN_SERIAL "TRUE" TIMEOUT "210" _BACKTRACE_TRIPLES "/home/es/xv6/xv6-riscv/test/cmake/QemuTests.cmake;37;add_test;/home/es/xv6/xv6-riscv/test/cmake/QemuTests.cmake;78;add_qemu_test;/home/es/xv6/xv6-riscv/test/cmake/QemuTests.cmake;0;;/home/es/xv6/xv6-riscv/test/CMakeLists.txt;387;include;/home/es/xv6/xv6-riscv/test/CMakeLists.txt;0;")
add_test(qemu_pcache "/home/es/xv6/xv6-riscv/test/../scripts/run_qemu_test.sh" "/home/es/xv6/xv6-riscv/test/build_qemu_pcache" "150" "PCACHE TESTS: [0-9]+/[0-9]+ PASSED" "PCACHE TESTS: [0-9]+/[0-9]+ FAILED" "--" "PCACHE_TEST=1")
set_tests_properties(qemu_pcache PROPERTIES  LABELS "qemu;slow" RESOURCE_LOCK "qemu_vm" RUN_SERIAL "TRUE" TIMEOUT "270" _BACKTRACE_TRIPLES "/home/es/xv6/xv6-riscv/test/cmake/QemuTests.cmake;37;add_test;/home/es/xv6/xv6-riscv/test/cmake/QemuTests.cmake;94;add_qemu_test;/home/es/xv6/xv6-riscv/test/cmake/QemuTests.cmake;0;;/home/es/xv6/xv6-riscv/test/CMakeLists.txt;387;include;/home/es/xv6/xv6-riscv/test/CMakeLists.txt;0;")
add_test(qemu_workqueue "/home/es/xv6/xv6-riscv/test/../scripts/run_qemu_test.sh" "/home/es/xv6/xv6-riscv/test/build_qemu_workqueue" "90" "WORKQUEUE TESTS: [0-9]+/[0-9]+ PASSED" "WORKQUEUE TESTS: [0-9]+/[0-9]+ FAILED" "--" "WORKQUEUE_TEST=1")
set_tests_properties(qemu_workqueue PROPERTIES  LABELS "qemu;slow" RESOURCE_LOCK "qemu_vm" RUN_SERIAL "TRUE" TIMEOUT "210" _BACKTRACE_TRIPLES "/home/es/xv6/xv6-riscv/test/cmake/QemuTests.cmake;37;add_test;/home/es/xv6/xv6-riscv/test/cmake/QemuTests.cmake;103;add_qemu_test;/home/es/xv6/xv6-riscv/test/cmake/QemuTests.cmake;0;;/home/es/xv6/xv6-riscv/test/CMakeLists.txt;387;include;/home/es/xv6/xv6-riscv/test/CMakeLists.txt;0;")
subdirs("_deps/cmocka-build")
