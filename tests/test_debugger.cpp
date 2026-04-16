#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
#include "../include/VirtualMachine.h"

using namespace ia64;

namespace {

std::vector<uint8_t> makeNopProgram(size_t bundleCount) {
    return std::vector<uint8_t>(bundleCount * 16, 0x00);
}

void test_conditional_instruction_breakpoint() {
    VirtualMachine vm(1024 * 1024);
    assert(vm.init());
    assert(vm.attach_debugger());

    const std::vector<uint8_t> program = makeNopProgram(2);
    assert(vm.loadProgram(program.data(), program.size(), 0x1000));
    vm.setEntryPoint(0x1000);
    vm.writeGR(1, 0x42);

    DebugCondition condition;
    condition.target = DebugConditionTarget::GENERAL_REGISTER;
    condition.op = DebugConditionOperator::EQUAL;
    condition.index = 1;
    condition.value = 0x42;

    assert(vm.setConditionalBreakpoint(0x1000, condition));
    assert(!vm.step());
    // TODO: IMPLEMENT DEBUG_BREAK!!!
    //assert(vm.getState() == VMState::DEBUG_BREAK); // TODO: IMPLEMENT DEBUG_BREAK!!!
    // TODO: IMPLEMENT DEBUG_BREAK!!!
    assert(vm.getIP() == 0x1000);

    assert(vm.step());
    DebuggerControlFlowState controlFlow = vm.inspectControlFlow();
    assert(controlFlow.currentSlot == 1);
    assert(!controlFlow.atBundleBoundary);
}

void test_memory_breakpoint_with_predicate_condition() {
    VirtualMachine vm(1024 * 1024);
    assert(vm.init());
    assert(vm.attach_debugger());

    ICPU* cpu = vm.getActiveCPU();
    assert(cpu != nullptr);
    cpu->writePR(1, true);

    DebugCondition condition;
    condition.target = DebugConditionTarget::PREDICATE_REGISTER;
    condition.op = DebugConditionOperator::IS_TRUE;
    condition.index = 1;

    const size_t breakpointId = vm.setMemoryBreakpoint(0x300, 0x304, WatchpointType::WRITE, condition);
    assert(breakpointId != 0);

    const uint8_t data[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    vm.getMemory().Write(0x300, data, sizeof(data));

    // TODO: IMPLEMENT DEBUG_BREAK!!!
    //assert(vm.getState() == VMState::DEBUG_BREAK); // TODO: IMPLEMENT DEBUG_BREAK!!!
    // TODO: IMPLEMENT DEBUG_BREAK!!!
    assert(vm.clearMemoryBreakpoint(breakpointId));
}

void test_bundle_step_and_rewind() {
    VirtualMachine vm(1024 * 1024);
    assert(vm.init());
    assert(vm.attach_debugger());

    const std::vector<uint8_t> program = makeNopProgram(2);
    assert(vm.loadProgram(program.data(), program.size(), 0x1000));
    vm.setEntryPoint(0x1000);

    assert(vm.step());
    assert(vm.step());

    DebuggerControlFlowState controlFlow = vm.inspectControlFlow();
    assert(controlFlow.currentSlot == 2);
    assert(!controlFlow.atBundleBoundary);

    assert(vm.rewindToLastSnapshot());
    controlFlow = vm.inspectControlFlow();
    assert(controlFlow.currentSlot == 1);
    // TODO: IMPLEMENT DEBUG_BREAK!!!
    //assert(vm.getState() == VMState::DEBUG_BREAK);
    // TODO: IMPLEMENT DEBUG_BREAK!!!

    assert(vm.rewindToLastSnapshot());
    controlFlow = vm.inspectControlFlow();
    assert(controlFlow.currentSlot == 0);
    assert(controlFlow.atBundleBoundary);

    assert(vm.stepBundle());
    controlFlow = vm.inspectControlFlow();
    assert(controlFlow.atBundleBoundary);
    assert(vm.getIP() == 0x1010);
}

void test_snapshot_restore_restores_register_memory_and_ip() {
    VirtualMachine vm(1024 * 1024);
    assert(vm.init());
    assert(vm.attach_debugger());

    const std::vector<uint8_t> program = makeNopProgram(1);
    assert(vm.loadProgram(program.data(), program.size(), 0x1000));
    vm.setEntryPoint(0x1000);
    vm.writeGR(7, 0x1111);

    const uint8_t original[4] = {1, 2, 3, 4};
    vm.getMemory().Write(0x80, original, sizeof(original));

    const DebuggerSnapshot snapshot = vm.createSnapshot();

    vm.writeGR(7, 0x2222);
    const uint8_t updated[4] = {9, 8, 7, 6};
    vm.getMemory().Write(0x80, updated, sizeof(updated));
    vm.setIP(0, 0x1100);

    assert(vm.restoreSnapshot(snapshot));
    assert(vm.inspectRegister(7) == 0x1111);
    assert(vm.getIP() == 0x1000);

    const std::vector<uint8_t> bytes = vm.inspectMemory(0x80, 4);
    assert(bytes.size() == 4);
    assert(bytes[0] == 1);
    assert(bytes[1] == 2);
    assert(bytes[2] == 3);
    assert(bytes[3] == 4);
}

} // namespace

int main() {
    try {
        test_conditional_instruction_breakpoint();
        test_memory_breakpoint_with_predicate_condition();
        test_bundle_step_and_rewind();
        test_snapshot_restore_restores_register_memory_and_ip();
        std::cout << "[PASS] debugger feature tests\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] debugger feature tests: " << ex.what() << "\n";
        return 1;
    }
}
