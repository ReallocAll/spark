#include "core/util/state_file.h"

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace spark {

namespace {

void setFilesystemError(std::string_view action, const std::error_code &error, std::string &out)
{
    out = std::string(action);
    if (error) {
        out += ": ";
        out += error.message();
    }
}

void removeTemporaryFile(const std::filesystem::path &path)
{
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

bool replaceFile(const std::filesystem::path &temporary, const std::filesystem::path &destination, std::string &error)
{
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error = "Unable to replace state file: " + std::to_string(GetLastError());
        return false;
    }
    return true;
#else
    std::error_code ec;
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        setFilesystemError("Unable to replace state file", ec, error);
        return false;
    }
    return true;
#endif
}

}  // namespace

bool readStateFile(const std::filesystem::path &path, std::size_t max_bytes, std::string &out, std::string &error)
{
    out.clear();

    std::error_code ec;
    const std::uintmax_t file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        setFilesystemError("Unable to inspect state file", ec, error);
        return false;
    }
    if (file_size > static_cast<std::uintmax_t>(max_bytes)) {
        error = "State file exceeds the maximum size";
        return false;
    }
    if (file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        error = "State file is too large to read";
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Unable to open state file for reading";
        return false;
    }

    const auto byte_count = static_cast<std::streamsize>(file_size);
    std::string text(static_cast<std::size_t>(file_size), '\0');
    if (byte_count > 0) {
        in.read(text.data(), byte_count);
        if (in.gcount() != byte_count || in.fail()) {
            error = "Unable to read state file";
            return false;
        }
    }

    const std::uintmax_t final_size = std::filesystem::file_size(path, ec);
    if (ec) {
        setFilesystemError("Unable to inspect state file after reading", ec, error);
        return false;
    }
    if (final_size != file_size) {
        error = "State file changed while reading";
        return false;
    }

    const int next = in.peek();
    if (next != EOF || in.bad()) {
        error = "State file changed while reading";
        return false;
    }
    in.clear();
    in.close();
    if (in.fail()) {
        error = "Unable to close state file after reading";
        return false;
    }

    out = std::move(text);
    error.clear();
    return true;
}

bool writeStateFileAtomically(const std::filesystem::path &path, std::string_view data, std::string &error)
{
    error.clear();
    const std::filesystem::path directory = path.parent_path();
    std::error_code ec;
    if (!directory.empty()) {
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            setFilesystemError("Unable to create state-file directory", ec, error);
            return false;
        }
    }

    std::filesystem::path temporary = path;
    temporary += ".tmp";
    if (data.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        error = "State file is too large to write";
        return false;
    }

    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Unable to open temporary state file for writing";
            removeTemporaryFile(temporary);
            return false;
        }
        if (!data.empty()) {
            out.write(data.data(), static_cast<std::streamsize>(data.size()));
        }
        if (!out) {
            error = "Unable to write temporary state file";
            out.close();
            removeTemporaryFile(temporary);
            return false;
        }
        out.flush();
        if (!out) {
            error = "Unable to flush temporary state file";
            out.close();
            removeTemporaryFile(temporary);
            return false;
        }
        out.close();
        if (!out) {
            error = "Unable to close temporary state file";
            removeTemporaryFile(temporary);
            return false;
        }
    }

    if (!replaceFile(temporary, path, error)) {
        removeTemporaryFile(temporary);
        return false;
    }
    return true;
}

}  // namespace spark
