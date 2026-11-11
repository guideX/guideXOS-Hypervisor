#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace riscv_linux_test {

struct LinuxConsoleLine {
    std::string text;
    std::vector<std::string> categories;
    std::string initializationEvidence = "message_only";
    std::string severity = "informational";
};

struct LinuxConsoleTranscript {
    uint64_t byteCount = 0;
    std::vector<LinuxConsoleLine> completeLines;
    std::string partialTrailingLine;
};

inline std::string LowerConsoleLine(const std::string& line) {
    std::string result = line;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return result;
}

inline bool ContainsAny(const std::string& line,
                        std::initializer_list<const char*> needles) {
    return std::any_of(needles.begin(), needles.end(), [&](const char* needle) {
        return line.find(needle) != std::string::npos;
    });
}

inline LinuxConsoleLine AnalyzeLinuxConsoleLine(std::string text) {
    LinuxConsoleLine result;
    result.text = std::move(text);
    const std::string lower = LowerConsoleLine(result.text);
    const auto addCategory = [&](const char* category) {
        result.categories.emplace_back(category);
    };

    if (result.text.find("Linux version ") != std::string::npos)
        addCategory("linux_version");
    if (result.text.find("Kernel command line:") != std::string::npos)
        addCategory("command_line");
    if (ContainsAny(lower, {"reserved mem:", "zone ranges:", "early memory node ranges",
                            "initmem setup", "memory:", "pages in unavailable ranges",
                            "virtual kernel memory layout", "memblock"}))
        addCategory("memory");
    if (lower.find("reserved mem:") != std::string::npos)
        addCategory("reserved_memory");
    if (ContainsAny(lower, {"clocksource:", "sched_clock:", "timekeeping", "time_init",
                            "clockevent", "timer"}))
        addCategory("clocksource_or_timer");
    if (ContainsAny(lower, {"nr_irqs:", "riscv-intc", "irqchip", "interrupt controller",
                            "plic", "irq domain"}))
        addCategory("interrupt_controller");
    if (ContainsAny(lower, {"earlycon:", "bootconsole", "console:", "ttys", "serial"}))
        addCategory("serial_console");
    if (ContainsAny(lower, {"scheduler", "sched_", "rcu", "cpuidle", "bogomips"}))
        addCategory("scheduler_related");
    if (ContainsAny(lower, {"devtmpfs", "pinctrl", "dma:", "thermal_sys", "hugetlb",
                            "iommu", "usbcore", "advanced linux sound", "vgaarb",
                            "pci:", "workingset", "futex"}))
        addCategory("driver_or_subsystem_initialization");
    if (lower.find("initcall") != std::string::npos)
        addCategory("initcall");

    const bool panic = ContainsAny(lower, {"kernel panic", "panic", "oops", "bug:"});
    const bool warningOrError = ContainsAny(lower, {"warning", "warn_once", "error",
                                                     " failed", "failure", "unable to",
                                                     "no working init"});
    if (panic) addCategory("kernel_bug_oops_or_panic");
    if (warningOrError) addCategory("warning_or_error");

    if (panic) {
        result.severity = "kernel_terminal_marker";
    } else if (warningOrError) {
        result.severity = "warning_or_error_marker";
    }

    if (ContainsAny(lower, {" initialized", " registered", " mapped", " detected",
                            " enabled", "switched to", " loaded", "built ",
                            "earlycon:", "clocksource:", "sched_clock:"})) {
        result.initializationEvidence = "success_reported_by_guest";
    } else if (ContainsAny(lower, {"initializing", "probing", "setting up", " setup ",
                                   "attempting"})) {
        result.initializationEvidence = "attempt_reported_by_guest";
    }
    return result;
}

inline LinuxConsoleTranscript ParseLinuxConsoleTranscript(
    const std::vector<uint8_t>& bytes) {
    LinuxConsoleTranscript result;
    result.byteCount = bytes.size();
    std::string line;
    for (const uint8_t byte : bytes) {
        if (byte == static_cast<uint8_t>('\n')) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            result.completeLines.push_back(AnalyzeLinuxConsoleLine(std::move(line)));
            line.clear();
        } else {
            line.push_back(static_cast<char>(byte));
        }
    }
    result.partialTrailingLine = std::move(line);
    return result;
}

}  // namespace riscv_linux_test
