#define UNW_LOCAL_ONLY
#include "native/alloc/linux_permanent_gateway_unwind.h"

#include <libunwind.h>
#include <unwind.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <sys/mman.h>

#include "native/alloc/linux_elf_admission.h"

// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-identifier-naming)
extern "C" __attribute__((visibility("hidden"))) unw_word_t _U_dyn_info_list_addr();

namespace spark::gateway::permanent {
// NOLINTBEGIN(performance-no-int-to-ptr)
namespace {

struct Bases {
    void *text;
    void *data;
    void *function;
};

using Register = void (*)(const void *);
using Find = const void *(*)(const void *, Bases *);

const void *fde(const Directory &directory, std::size_t index)
{
    return reinterpret_cast<const void *>(directory.metadata + KCieSize + KFdeStride * index);
}

bool lookup(const Directory &directory, std::size_t index, bool present, Deadline deadline = Deadline::max())
{
    const auto find = reinterpret_cast<Find>(directory.host.functions[2]);
    for (auto offset : {std::uintptr_t{1}, std::uintptr_t{KCodeStride / 2}, std::uintptr_t{KCodeStride - 1}}) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        Bases bases{};
        const auto start = directory.code + KCodeStride * index;
        const void *result = find(reinterpret_cast<const void *>(start + offset), &bases);
        if (present ? result != fde(directory, index) || reinterpret_cast<std::uintptr_t>(bases.function) != start
                    : result != nullptr) {
            return false;
        }
    }
    return true;
}

bool expired(Deadline deadline)
{
    return std::chrono::steady_clock::now() >= deadline;
}

struct Transaction {
    const Directory &directory;
    Deadline deadline;
    bool dirty = false;
    std::array<const void *, KEntryCount> calls{};
    std::size_t count = 0;

    void add(const void *address)
    {
        elf::require(count < calls.size() && !expired(deadline), "host registration capacity or deadline exceeded");
        reinterpret_cast<Register>(directory.host.functions[0])(address);
        calls[count++] = address;
        dirty = true;
#if defined(SPARK_GATEWAY_TESTING)
        ++reinterpret_cast<State *>(directory.state)->host_registrations;
#endif
    }

    void removeLast()
    {
        reinterpret_cast<Register>(directory.host.functions[1])(calls[count - 1]);
        --count;
    }

    bool rollback()
    {
        if (!dirty) {
            return true;
        }
#if defined(SPARK_GATEWAY_TESTING)
        if (std::getenv("SPARK_GATEWAY_UNPROVEN_ROLLBACK") != nullptr) {
            return false;
        }
#endif
        while (count != 0) {
            if (expired(deadline)) {
                return false;
            }
            removeLast();
        }
        for (std::size_t i = 0; i < KEntryCount; ++i) {
            if (!lookup(directory, i, false, deadline)) {
                return false;
            }
        }
        dirty = false;
        return true;
    }
};

struct PrivateRegistration {
    unw_dyn_info_t info{};
    bool registered = false;

    ~PrivateRegistration()
    {
        if (registered) {
            _U_dyn_cancel(&info);
        }
    }
};

PrivateRegistration Private;

void registrationFault(std::size_t index)
{
#if defined(SPARK_GATEWAY_TESTING)
    const char *value = std::getenv("SPARK_GATEWAY_REGISTRATION_FAULT");
    elf::require(value == nullptr || std::strtoul(value, nullptr, 10) != index, "injected host registration failure");
#else
    (void)index;
#endif
}

}  // namespace

HostUnwinder resolveHost(const void *anchor)
{
    elf::Admission admission(anchor);
    constexpr std::array names{"__register_frame", "__deregister_frame", "_Unwind_Find_FDE", "_Unwind_Backtrace",
                               "_Unwind_GetIP"};
    HostUnwinder result;
    std::size_t owner = admission.snapshot.objects.size();
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto *const address = ::dlsym(RTLD_DEFAULT, names[i]);
        elf::require(address != nullptr, "host unwind function is unavailable");
        const auto index = admission.snapshot.owner(address, PF_R | PF_X);
        elf::require(admission.resident.contains(index) && index != admission.spark &&
                         !elf::sparkObject(admission.snapshot.objects[index]),
                     "host unwinder is outside the admitted startup closure");
        if (i == 0) {
            owner = index;
        }
        elf::require(index == owner, "host unwind functions have mixed providers");
        const auto &object = admission.snapshot.objects[index];
        bool ordinary = false;
        for (const auto &symbol : object.symbols) {
            if (symbol.st_shndx != SHN_UNDEF && symbol.st_shndx < SHN_LORESERVE &&
                object.string(symbol.st_name) == names[i] && ELF64_ST_TYPE(symbol.st_info) == STT_FUNC &&
                ELF64_ST_BIND(symbol.st_info) == STB_GLOBAL &&
                elf::add(object.bias, symbol.st_value) == reinterpret_cast<std::uintptr_t>(address)) {
                ordinary = true;
            }
        }
        elf::require(ordinary, "host unwind symbol is not an ordinary definition");
        result.functions[i] = reinterpret_cast<std::uintptr_t>(address);
    }
    const auto &identity = admission.snapshot.objects[owner].identity;
    LoaderHandle handle(lease(identity));
    elf::require(static_cast<bool>(handle) && admission.snapshot.unchanged(), "cannot retain host unwinder");
    result.provider = {
        .base = identity.base, .device = identity.device, .inode = identity.inode, .handle = handle.release()};
    return result;
}

bool registerHost(Directory &directory, Deadline deadline, bool &quarantined)
{
    auto *probe_metadata =
        static_cast<unsigned char *>(::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (probe_metadata == MAP_FAILED) {
        return false;
    }
    std::memcpy(probe_metadata, reinterpret_cast<const void *>(directory.metadata), KCieSize + 2 * KFdeStride);
    if (::mprotect(probe_metadata, 4096, PROT_READ) != 0) {
        ::munmap(probe_metadata, 4096);
        return false;
    }
    Directory probe = directory;
    probe.metadata = reinterpret_cast<std::uintptr_t>(probe_metadata);
    Transaction transaction{.directory = probe, .deadline = deadline};
    try {
        for (std::size_t i = 0; i < KEntryCount; ++i) {
            elf::require(!expired(deadline) && lookup(directory, i, false, deadline),
                         "host unwind probe range is not absent");
        }
        transaction.add(fde(probe, 0));
        elf::require(lookup(probe, 0, true, deadline), "host unwind probe did not register its first FDE");
        if (lookup(probe, 1, true, deadline)) {
            directory.host.mode = HostRegistrationMode::Sequence;
        }
        else {
            elf::require(lookup(probe, 1, false, deadline), "host unwind probe returned an unexpected second FDE");
            directory.host.mode = HostRegistrationMode::SingleFde;
        }
        elf::require(transaction.rollback(), "host unwind discrimination cleanup failed");
        elf::require(!expired(deadline), "host unwind setup deadline expired");
        if (directory.host.mode == HostRegistrationMode::Sequence) {
            transaction.add(probe_metadata);
            elf::require(lookup(probe, 0, true, deadline) && lookup(probe, 1, true, deadline),
                         "host sequence confirmation failed");
        }
        else {
            transaction.add(fde(probe, 0));
            transaction.add(fde(probe, 1));
            elf::require(lookup(probe, 0, true, deadline) && lookup(probe, 1, true, deadline),
                         "host single-FDE confirmation failed");
            elf::require(!expired(deadline), "host unwind confirmation deadline expired");
            transaction.removeLast();
            elf::require(lookup(probe, 0, true, deadline) && lookup(probe, 1, false, deadline),
                         "host single-FDE removal failed");
        }
        elf::require(transaction.rollback(), "host unwind confirmation cleanup failed");
        ::munmap(probe_metadata, 4096);
        probe_metadata = nullptr;
        probe = directory;
        if (directory.host.mode == HostRegistrationMode::Sequence) {
            elf::require(!expired(deadline), "host unwind setup deadline expired");
            transaction.add(reinterpret_cast<const void *>(directory.metadata));
            registrationFault(0);
        }
        else {
            for (std::size_t i = 0; i < KEntryCount; ++i) {
                elf::require(!expired(deadline), "host unwind setup deadline expired");
                transaction.add(fde(directory, i));
                registrationFault(i);
                elf::require(lookup(directory, i, true, deadline), "host unwind registration verification failed");
            }
        }
        elf::require(verifyHost(directory, deadline), "host unwind arena verification failed");
        return true;
    }
    catch (...) {
        quarantined = !transaction.rollback();
        if (!quarantined && probe_metadata != nullptr) {
            ::munmap(probe_metadata, 4096);
        }
        return false;
    }
}

bool unregisterHost(const Directory &directory, Deadline deadline)
{
    if (expired(deadline)) {
        return false;
    }
    const auto deregister = reinterpret_cast<Register>(directory.host.functions[1]);
    if (directory.host.mode == HostRegistrationMode::Sequence) {
        deregister(reinterpret_cast<const void *>(directory.metadata));
    }
    else if (directory.host.mode == HostRegistrationMode::SingleFde) {
        for (std::size_t i = KEntryCount; i != 0; --i) {
            if (expired(deadline)) {
                return false;
            }
            deregister(fde(directory, i - 1));
        }
    }
    else {
        return false;
    }
    for (std::size_t i = 0; i < KEntryCount; ++i) {
        if (!lookup(directory, i, false, deadline)) {
            return false;
        }
    }
    return true;
}

void unregisterPrivate()
{
    if (Private.registered) {
        _U_dyn_cancel(&Private.info);
        Private.registered = false;
    }
}

bool verifyHost(const Directory &directory, Deadline deadline)
{
    if (directory.host.version != 1 || (directory.host.mode != HostRegistrationMode::Sequence &&
                                        directory.host.mode != HostRegistrationMode::SingleFde)) {
        return false;
    }
    for (std::size_t i = 0; i < KEntryCount; ++i) {
        if (!lookup(directory, i, true, deadline)) {
            return false;
        }
    }
    return true;
}

bool registerPrivate(const Directory &directory, const void *anchor)
{
    Identity owner;
    if (!identify(anchor, owner)) {
        return false;
    }
    for (const auto *const address :
         {reinterpret_cast<const void *>(&_U_dyn_register), reinterpret_cast<const void *>(&_U_dyn_cancel),
          reinterpret_cast<const void *>(&_U_dyn_info_list_addr),
          reinterpret_cast<const void *>(&unw_get_proc_info_by_ip), reinterpret_cast<const void *>(&unw_step)}) {
        Identity actual;
        if (!identify(address, actual) || actual.base != owner.base || actual.device != owner.device ||
            actual.inode != owner.inode) {
            return false;
        }
    }
    if (Private.registered) {
        return Private.info.start_ip == directory.code;
    }
    auto &info = Private.info;
    info.start_ip = directory.code;
    info.end_ip = directory.code + directory.code_size;
    info.format = UNW_INFO_FORMAT_REMOTE_TABLE;
    info.u.rti.segbase = directory.code;
    info.u.rti.table_data = directory.metadata + KTableOffset;
    info.u.rti.table_len = KEntryCount * 8 / sizeof(unw_word_t);
    _U_dyn_register(&info);
    Private.registered = true;
    bool found = false;
    auto *list = reinterpret_cast<unw_dyn_info_list_t *>(_U_dyn_info_list_addr());
    std::size_t count = 0;
    for (auto *node = list->first; node != nullptr && count++ < 4096; node = node->next) {
        found |= node == &info;
    }
    for (std::size_t i = 0; found && i < KEntryCount; ++i) {
        unw_proc_info_t procedure{};
        found = unw_get_proc_info_by_ip(unw_local_addr_space, directory.code + i * KCodeStride + 1, &procedure,
                                        nullptr) == 0 &&
                procedure.start_ip == directory.code + i * KCodeStride &&
                procedure.end_ip == directory.code + (i + 1) * KCodeStride;
    }
    if (!found) {
        _U_dyn_cancel(&info);
        Private.registered = false;
    }
    return found;
}

// NOLINTEND(performance-no-int-to-ptr)
}  // namespace spark::gateway::permanent
