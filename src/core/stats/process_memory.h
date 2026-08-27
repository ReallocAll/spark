#ifndef ENDSTONE_SPARK_PROCESS_MEMORY_H
#define ENDSTONE_SPARK_PROCESS_MEMORY_H

#include <cstdint>
#include <limits>

#ifdef _WIN32
// clang-format off: psapi.h requires windows.h types
#include <windows.h>
#include <psapi.h>
// clang-format on
#else
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <string>
#endif

namespace spark {

enum class ProcessMemoryLimitSource {
    None,
    CgroupV2MemoryMax,
    WindowsJobProcessMemory,
    WindowsJobMemory,
};

struct ProcessMemoryUsage {
    bool used_present = false;
    std::int64_t used_bytes = 0;
    bool committed_present = false;
    std::int64_t committed_bytes = 0;
    bool max_present = false;
    std::int64_t max_bytes = 0;
    ProcessMemoryLimitSource max_source = ProcessMemoryLimitSource::None;
};

inline ProcessMemoryUsage gatherProcessMemoryUsage()
{
    ProcessMemoryUsage result;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                             sizeof(counters))) {
        if (counters.WorkingSetSize <= static_cast<SIZE_T>(std::numeric_limits<std::int64_t>::max())) {
            result.used_bytes = static_cast<std::int64_t>(counters.WorkingSetSize);
            result.used_present = true;
        }
        if (counters.PrivateUsage <= static_cast<SIZE_T>(std::numeric_limits<std::int64_t>::max())) {
            result.committed_bytes = static_cast<std::int64_t>(counters.PrivateUsage);
            result.committed_present = true;
        }
    }

    BOOL in_job = FALSE;
    if (IsProcessInJob(GetCurrentProcess(), nullptr, &in_job) && in_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr)) {
            std::uint64_t memory_limit = 0;
            ProcessMemoryLimitSource limit_source = ProcessMemoryLimitSource::None;
            if ((limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) != 0) {
                memory_limit = static_cast<std::uint64_t>(limits.ProcessMemoryLimit);
                limit_source = ProcessMemoryLimitSource::WindowsJobProcessMemory;
            }
            if ((limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) != 0) {
                const std::uint64_t job_limit = static_cast<std::uint64_t>(limits.JobMemoryLimit);
                if (memory_limit == 0 || (job_limit > 0 && job_limit < memory_limit)) {
                    memory_limit = job_limit;
                    limit_source = ProcessMemoryLimitSource::WindowsJobMemory;
                }
            }
            if (memory_limit > 0 &&
                memory_limit <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                result.max_bytes = static_cast<std::int64_t>(memory_limit);
                result.max_present = true;
                result.max_source = limit_source;
            }
        }
    }
#else
    std::ifstream statm("/proc/self/statm");
    std::int64_t pages_total = 0;
    std::int64_t pages_resident = 0;
    const std::int64_t page_size = static_cast<std::int64_t>(sysconf(_SC_PAGESIZE));
    if (page_size > 0 && statm >> pages_total >> pages_resident && pages_resident >= 0 &&
        pages_resident <= std::numeric_limits<std::int64_t>::max() / page_size) {
        result.used_bytes = pages_resident * page_size;
        result.used_present = true;
    }

    std::ifstream cgroup("/proc/self/cgroup");
    std::string line;
    std::string cgroup_path;
    while (std::getline(cgroup, line)) {
        if (line.starts_with("0::")) {
            cgroup_path = line.substr(3);
            break;
        }
    }
    if (!cgroup_path.empty()) {
        std::string memory_max_path = "/sys/fs/cgroup";
        if (cgroup_path != "/") {
            memory_max_path += cgroup_path;
        }
        memory_max_path += "/memory.max";
        std::ifstream memory_max(memory_max_path);
        std::string value;
        if (memory_max >> value && value != "max") {
            char *end = nullptr;
            const std::uint64_t parsed = static_cast<std::uint64_t>(std::strtoull(value.c_str(), &end, 10));
            if (end != nullptr && end != value.c_str() && *end == '\0' && parsed > 0 &&
                parsed <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                result.max_bytes = static_cast<std::int64_t>(parsed);
                result.max_present = true;
                result.max_source = ProcessMemoryLimitSource::CgroupV2MemoryMax;
            }
        }
    }
#endif
    return result;
}

}  // namespace spark

#endif  // ENDSTONE_SPARK_PROCESS_MEMORY_H
