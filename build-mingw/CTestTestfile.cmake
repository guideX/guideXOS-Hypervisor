# CMake generated Testfile for 
# Source directory: D:/dev/guideXOS_Emulator
# Build directory: D:/dev/guideXOS_Emulator/build-mingw
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(BasicTests "D:/dev/guideXOS_Emulator/build-mingw/bin/ia64emu_tests.exe")
set_tests_properties(BasicTests PROPERTIES  _BACKTRACE_TRIPLES "D:/dev/guideXOS_Emulator/CMakeLists.txt;540;add_test;D:/dev/guideXOS_Emulator/CMakeLists.txt;0;")
add_test(NewInstructionTests "D:/dev/guideXOS_Emulator/build-mingw/bin/test_new_instructions.exe")
set_tests_properties(NewInstructionTests PROPERTIES  _BACKTRACE_TRIPLES "D:/dev/guideXOS_Emulator/CMakeLists.txt;541;add_test;D:/dev/guideXOS_Emulator/CMakeLists.txt;0;")
add_test(SyscallTests "D:/dev/guideXOS_Emulator/build-mingw/bin/test_syscall_dispatcher.exe")
set_tests_properties(SyscallTests PROPERTIES  _BACKTRACE_TRIPLES "D:/dev/guideXOS_Emulator/CMakeLists.txt;542;add_test;D:/dev/guideXOS_Emulator/CMakeLists.txt;0;")
add_test(DebuggerTests "D:/dev/guideXOS_Emulator/build-mingw/bin/test_debugger.exe")
set_tests_properties(DebuggerTests PROPERTIES  _BACKTRACE_TRIPLES "D:/dev/guideXOS_Emulator/CMakeLists.txt;543;add_test;D:/dev/guideXOS_Emulator/CMakeLists.txt;0;")
add_test(ProfilerTests "D:/dev/guideXOS_Emulator/build-mingw/bin/test_profiler.exe")
set_tests_properties(ProfilerTests PROPERTIES  _BACKTRACE_TRIPLES "D:/dev/guideXOS_Emulator/CMakeLists.txt;544;add_test;D:/dev/guideXOS_Emulator/CMakeLists.txt;0;")
add_test(FuzzerTests "D:/dev/guideXOS_Emulator/build-mingw/bin/test_fuzzer.exe")
set_tests_properties(FuzzerTests PROPERTIES  _BACKTRACE_TRIPLES "D:/dev/guideXOS_Emulator/CMakeLists.txt;545;add_test;D:/dev/guideXOS_Emulator/CMakeLists.txt;0;")
add_test(VMIsolationTests "D:/dev/guideXOS_Emulator/build-mingw/bin/test_vm_isolation.exe")
set_tests_properties(VMIsolationTests PROPERTIES  _BACKTRACE_TRIPLES "D:/dev/guideXOS_Emulator/CMakeLists.txt;546;add_test;D:/dev/guideXOS_Emulator/CMakeLists.txt;0;")
add_test(RiscVDecoderSmokeTest "D:/dev/guideXOS_Emulator/build-mingw/bin/test_riscv_decoder.exe")
set_tests_properties(RiscVDecoderSmokeTest PROPERTIES  _BACKTRACE_TRIPLES "D:/dev/guideXOS_Emulator/CMakeLists.txt;547;add_test;D:/dev/guideXOS_Emulator/CMakeLists.txt;0;")
add_test(ELFValidationTests "D:/dev/guideXOS_Emulator/build-mingw/bin/test_elf_validation.exe")
set_tests_properties(ELFValidationTests PROPERTIES  _BACKTRACE_TRIPLES "D:/dev/guideXOS_Emulator/CMakeLists.txt;548;add_test;D:/dev/guideXOS_Emulator/CMakeLists.txt;0;")
