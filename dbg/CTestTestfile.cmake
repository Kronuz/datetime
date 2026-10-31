# CMake generated Testfile for 
# Source directory: /Users/gmendezb/code/Kronuz/libs/datetime
# Build directory: /Users/gmendezb/code/Kronuz/libs/datetime/dbg
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(datetime "/Users/gmendezb/code/Kronuz/libs/datetime/dbg/datetime_test")
set_tests_properties(datetime PROPERTIES  _BACKTRACE_TRIPLES "/Users/gmendezb/code/Kronuz/libs/datetime/CMakeLists.txt;44;add_test;/Users/gmendezb/code/Kronuz/libs/datetime/CMakeLists.txt;0;")
add_test(datetime_diff "/Users/gmendezb/code/Kronuz/libs/datetime/dbg/datetime_diff_test" "20000")
set_tests_properties(datetime_diff PROPERTIES  _BACKTRACE_TRIPLES "/Users/gmendezb/code/Kronuz/libs/datetime/CMakeLists.txt;52;add_test;/Users/gmendezb/code/Kronuz/libs/datetime/CMakeLists.txt;0;")
add_test(datetime_datemath "/Users/gmendezb/code/Kronuz/libs/datetime/dbg/datetime_datemath_test" "20000")
set_tests_properties(datetime_datemath PROPERTIES  _BACKTRACE_TRIPLES "/Users/gmendezb/code/Kronuz/libs/datetime/CMakeLists.txt;58;add_test;/Users/gmendezb/code/Kronuz/libs/datetime/CMakeLists.txt;0;")
subdirs("_deps/strict_stox-build")
