# CMake generated Testfile for 
# Source directory: /home/es/xv6/xv6-riscv/test
# Build directory: /home/es/xv6/xv6-riscv/test/build_host
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(ut_list "/home/es/xv6/xv6-riscv/test/build_host/ut_list")
set_tests_properties(ut_list PROPERTIES  WORKING_DIRECTORY "/home/es/xv6/xv6-riscv/test/build_host" _BACKTRACE_TRIPLES "/home/es/xv6/xv6-riscv/test/CMakeLists.txt;321;add_test;/home/es/xv6/xv6-riscv/test/CMakeLists.txt;0;")
add_test(ut_bits "/home/es/xv6/xv6-riscv/test/build_host/ut_bits")
set_tests_properties(ut_bits PROPERTIES  WORKING_DIRECTORY "/home/es/xv6/xv6-riscv/test/build_host" _BACKTRACE_TRIPLES "/home/es/xv6/xv6-riscv/test/CMakeLists.txt;329;add_test;/home/es/xv6/xv6-riscv/test/CMakeLists.txt;0;")
add_test(ut_workqueue "/home/es/xv6/xv6-riscv/test/build_host/ut_workqueue")
set_tests_properties(ut_workqueue PROPERTIES  WORKING_DIRECTORY "/home/es/xv6/xv6-riscv/test/build_host" _BACKTRACE_TRIPLES "/home/es/xv6/xv6-riscv/test/CMakeLists.txt;336;add_test;/home/es/xv6/xv6-riscv/test/CMakeLists.txt;0;")
add_test(rust_host_unit_tests "/usr/bin/cargo" "test" "--target" "x86_64-unknown-linux-gnu")
set_tests_properties(rust_host_unit_tests PROPERTIES  ENVIRONMENT "LAB=fs" WORKING_DIRECTORY "/home/es/xv6/xv6-riscv/test/../kernel" _BACKTRACE_TRIPLES "/home/es/xv6/xv6-riscv/test/CMakeLists.txt;371;add_test;/home/es/xv6/xv6-riscv/test/CMakeLists.txt;0;")
subdirs("_deps/cmocka-build")
