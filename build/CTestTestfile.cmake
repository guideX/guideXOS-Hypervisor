# CMake generated Testfile for 
# Source directory: D:/devgitlab/guideXOS.Hypervisor/guideXOS Hypervisor
# Build directory: D:/devgitlab/guideXOS.Hypervisor/guideXOS Hypervisor/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(BasicTests "D:/devgitlab/guideXOS.Hypervisor/guideXOS Hypervisor/build/bin/Debug/ia64emu_tests.exe")
  set_tests_properties(BasicTests PROPERTIES  _BACKTRACE_TRIPLES "D:/devgitlab/guideXOS.Hypervisor/guideXOS Hypervisor/CMakeLists.txt;137;add_test;D:/devgitlab/guideXOS.Hypervisor/guideXOS Hypervisor/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(BasicTests "D:/devgitlab/guideXOS.Hypervisor/guideXOS Hypervisor/build/bin/Release/ia64emu_tests.exe")
  set_tests_properties(BasicTests PROPERTIES  _BACKTRACE_TRIPLES "D:/devgitlab/guideXOS.Hypervisor/guideXOS Hypervisor/CMakeLists.txt;137;add_test;D:/devgitlab/guideXOS.Hypervisor/guideXOS Hypervisor/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test(BasicTests "D:/devgitlab/guideXOS.Hypervisor/guideXOS Hypervisor/build/bin/MinSizeRel/ia64emu_tests.exe")
  set_tests_properties(BasicTests PROPERTIES  _BACKTRACE_TRIPLES "D:/devgitlab/guideXOS.Hypervisor/guideXOS Hypervisor/CMakeLists.txt;137;add_test;D:/devgitlab/guideXOS.Hypervisor/guideXOS Hypervisor/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(BasicTests "D:/devgitlab/guideXOS.Hypervisor/guideXOS Hypervisor/build/bin/RelWithDebInfo/ia64emu_tests.exe")
  set_tests_properties(BasicTests PROPERTIES  _BACKTRACE_TRIPLES "D:/devgitlab/guideXOS.Hypervisor/guideXOS Hypervisor/CMakeLists.txt;137;add_test;D:/devgitlab/guideXOS.Hypervisor/guideXOS Hypervisor/CMakeLists.txt;0;")
else()
  add_test(BasicTests NOT_AVAILABLE)
endif()
