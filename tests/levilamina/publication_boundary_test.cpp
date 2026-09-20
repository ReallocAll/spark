#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "platform/levilamina/command_lifecycle.h"
#include "platform/levilamina/cleanup_deadline_guard.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using spark::levilamina::PublicationBoundary;

void require(bool condition, char const *message)
{
    if (!condition) {
        throw std::runtime_error{message};
    }
}

#ifdef _WIN32

constexpr DWORD kFailClosedExit = ERROR_TIMEOUT;

std::wstring currentExecutablePath()
{
    std::wstring path(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }
    path.resize(length);
    return path;
}

DWORD runChild(std::wstring const &path, wchar_t const *mode)
{
    std::wstring command = L"\"" + path + L"\" " + mode;
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    require(::CreateProcessW(path.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                             nullptr, &startup, &process) != FALSE,
            "fault oracle child failed to launch");
    ::CloseHandle(process.hThread);
    require(::WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0, "fault oracle child timed out");
    DWORD exit_code = STILL_ACTIVE;
    require(::GetExitCodeProcess(process.hProcess, &exit_code) != FALSE, "fault oracle exit code was unavailable");
    ::CloseHandle(process.hProcess);
    return exit_code;
}

int runChildMode(wchar_t const *mode)
{
    PublicationBoundary boundary;
    boundary.begin();
    if (std::wstring_view{mode} == L"--second-factory") {
        boundary.begin();
    }
    try {
        throw std::runtime_error{"synthetic post-publication failure"};
    }
    catch (...) {
        const bool returned = spark::levilamina::runPublicationFailurePath(
            boundary,
            std::current_exception(),
            [] {},
            [](std::exception_ptr) {},
            [] { spark::levilamina::CleanupDeadlineGuard::terminateOnTimeout(); }
        );
        return returned ? 1 : 2;
    }
    return 3;
}

#endif

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
{
    try {
        if (argc == 2) {
            if (argv[1] == nullptr ||
                (std::wstring_view{argv[1]} != L"--first-factory" &&
                 std::wstring_view{argv[1]} != L"--second-factory")) {
                return 64;
            }
            return runChildMode(argv[1]);
        }
        require(argc == 1, "fault oracle received unexpected arguments");

        PublicationBoundary prepublication;
        require(!prepublication.requiresFailClosed(), "pre-publication state was fail-closed");
        bool cleanup_called = false;
        bool report_called = false;
        require(!spark::levilamina::runPublicationFailurePath(
                    prepublication,
                    std::exception_ptr{},
                    [&] { cleanup_called = true; },
                    [&](std::exception_ptr) { report_called = true; },
                    [] { throw std::runtime_error{"unexpected pre-publication fail-closed"}; }
                ),
                "pre-publication failure did not return false");
        require(cleanup_called && report_called, "pre-publication failure did not run cleanup/report");

        PublicationBoundary published;
        published.begin();
        require(published.requiresFailClosed(), "first factory publication was not recorded");
        require(!published.confirmHostRegistryCleanup(false), "unclean host registry was accepted");
        require(published.requiresFailClosed(), "unclean host registry cleared publication state");
        require(published.confirmHostRegistryCleanup(true), "clean host registry was not accepted");
        require(!published.requiresFailClosed(), "clean host registry did not reopen publication");

        const auto path = currentExecutablePath();
        require(!path.empty(), "fault oracle executable path was unavailable");
        const auto first_exit = runChild(path, L"--first-factory");
        const auto second_exit = runChild(path, L"--second-factory");
        std::fprintf(stderr, "publication-boundary: first-exit=%lu second-exit=%lu expected=%lu\n",
                     static_cast<unsigned long>(first_exit), static_cast<unsigned long>(second_exit),
                     static_cast<unsigned long>(kFailClosedExit));
        require(first_exit == kFailClosedExit,
                "first-factory failure returned without controlled termination");
        require(second_exit == kFailClosedExit,
                "second-factory failure returned without controlled termination");
        return 0;
    }
    catch (...) {
        return 1;
    }
}
#else
int main()
{
    return 0;
}
#endif
