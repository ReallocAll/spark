#include "net/bytebin.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

#if defined(_WIN32)
#include <windows.h>

#include <filesystem>
#include <system_error>
#else
#include <unistd.h>
#endif

namespace spark {

namespace {

#if defined(_WIN32)

bool createTempFile(std::filesystem::path &path)
{
    wchar_t temp_dir[MAX_PATH + 1]{};
    DWORD length = GetTempPathW(MAX_PATH + 1, temp_dir);
    if (length == 0 || length > MAX_PATH) {
        return false;
    }

    wchar_t temp_file[MAX_PATH + 1]{};
    if (GetTempFileNameW(temp_dir, L"spk", 0, temp_file) == 0) {
        return false;
    }
    path = temp_file;
    return true;
}

std::wstring utf8ToWide(const std::string &value)
{
    if (value.empty()) {
        return {};
    }
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int size = MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        code_page = CP_ACP;
        flags = 0;
        size = MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (size <= 0) {
            return {};
        }
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), wide.data(), size);
    return wide;
}

std::wstring commandQuote(const std::wstring &value)
{
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++backslashes;
        }
        else {
            if (c == L'\"') {
                out.append(backslashes * 2 + 1, L'\\');
            }
            else {
                out.append(backslashes, L'\\');
            }
            backslashes = 0;
            if (c == L'^' || c == L'%') {
                out += L'^';
            }
            out += c;
        }
    }
    out.append(backslashes * 2, L'\\');
    out += L'\"';
    return out;
}

#else

std::string shellQuote(const std::string &s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        }
        else {
            out += c;
        }
    }
    out += "'";
    return out;
}

#endif

std::string toLower(std::string s)
{
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

}  // namespace

#if defined(_WIN32)

UploadResult uploadToBytebin(const std::string &gzipped_body, const std::string &bytebin_url,
                             const std::string &content_type, const std::string &user_agent)
{
    UploadResult result;

    std::filesystem::path tmpfile;
    if (!createTempFile(tmpfile)) {
        result.error = "failed to create temp file";
        return result;
    }
    {
        std::ofstream output(tmpfile, std::ios::binary | std::ios::trunc);
        output.write(gzipped_body.data(), static_cast<std::streamsize>(gzipped_body.size()));
        output.close();
        if (!output) {
            std::error_code ec;
            std::filesystem::remove(tmpfile, ec);
            result.error = "failed to write temp file";
            return result;
        }
    }

    std::string url = bytebin_url;
    if (url.empty() || url.back() != '/') {
        url += '/';
    }
    url += "post";

    std::filesystem::path err_file;
    bool has_err_file = createTempFile(err_file);

    std::wstring cmd = L"C:\\Windows\\System32\\curl.exe -s -S -X POST --data-binary @" + commandQuote(tmpfile.wstring()) + L" -H " +
                       commandQuote(utf8ToWide("Content-Type: " + content_type)) + L" -H " +
                       commandQuote(L"Content-Encoding: gzip") + L" -H " +
                       commandQuote(utf8ToWide("User-Agent: " + user_agent)) + L" -D - -o NUL " +
                       commandQuote(utf8ToWide(url));
    if (has_err_file) {
        cmd += L" 2>" + commandQuote(err_file.wstring());
    }

    FILE *pipe = ::_wpopen(cmd.c_str(), L"r");
    if (pipe == nullptr) {
        std::error_code ec;
        std::filesystem::remove(tmpfile, ec);
        if (has_err_file) {
            std::filesystem::remove(err_file, ec);
        }
        result.error = "failed to run curl";
        return result;
    }
    std::string headers;
    char buffer[4096];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        headers.append(buffer, n);
    }
    int status = ::_pclose(pipe);
    std::error_code ec;
    std::filesystem::remove(tmpfile, ec);

    std::string curl_err;
    if (has_err_file) {
        std::ifstream ef(err_file);
        curl_err.assign(std::istreambuf_iterator<char>(ef), std::istreambuf_iterator<char>());
        std::filesystem::remove(err_file, ec);
        while (!curl_err.empty() && (curl_err.back() == '\n' || curl_err.back() == '\r' || curl_err.back() == ' ')) {
            curl_err.pop_back();
        }
    }

    if (status != 0) {
        result.error = curl_err.empty() ? "curl failed (is curl installed and reachable?)" : ("curl: " + curl_err);
        return result;
    }

    std::string key;
    std::size_t pos = 0;
    while (pos < headers.size()) {
        std::size_t eol = headers.find('\n', pos);
        std::string line = headers.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? headers.size() : eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (toLower(line).rfind("location:", 0) == 0) {
            std::string value = line.substr(9);
            std::size_t start = value.find_first_not_of(" \t");
            if (start != std::string::npos) {
                value = value.substr(start);
            }
            std::size_t slash = value.find_last_of('/');
            key = slash == std::string::npos ? value : value.substr(slash + 1);
        }
    }

    if (key.empty()) {
        result.error = "bytebin did not return a content key";
        return result;
    }
    result.ok = true;
    result.key = key;
    return result;
}

#else

UploadResult uploadToBytebin(const std::string &gzipped_body, const std::string &bytebin_url,
                             const std::string &content_type, const std::string &user_agent)
{
    UploadResult result;

    char tmpl[] = "/tmp/endstone-spark-XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) {
        result.error = "failed to create temp file";
        return result;
    }
    std::string tmpfile = tmpl;
    ssize_t written = ::write(fd, gzipped_body.data(), gzipped_body.size());
    ::close(fd);
    if (written != static_cast<ssize_t>(gzipped_body.size())) {
        ::unlink(tmpfile.c_str());
        result.error = "failed to write temp file";
        return result;
    }

    std::string url = bytebin_url;
    if (url.empty() || url.back() != '/') {
        url += '/';
    }
    url += "post";

    // Capture curl's stderr so failures are actionable, and force common bin dirs onto
    // PATH — a server process may be launched with a minimal environment.
    char err_template[] = "/tmp/endstone-spark-err-XXXXXX";
    int err_fd = ::mkstemp(err_template);
    std::string err_file;
    if (err_fd >= 0) {
        ::close(err_fd);
        err_file = err_template;
    }

    std::string cmd = "PATH=\"$PATH:/usr/bin:/usr/local/bin:/bin\" curl -s -S -X POST --data-binary @" +
                      shellQuote(tmpfile) + " -H " + shellQuote("Content-Type: " + content_type) + " -H " +
                      shellQuote("Content-Encoding: gzip") + " -H " + shellQuote("User-Agent: " + user_agent) +
                      " -D - -o /dev/null " + shellQuote(url);
    if (!err_file.empty()) {
        cmd += " 2>" + shellQuote(err_file);
    }

    FILE *pipe = ::popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        ::unlink(tmpfile.c_str());
        if (!err_file.empty()) {
            ::unlink(err_file.c_str());
        }
        result.error = "failed to run curl";
        return result;
    }
    std::string headers;
    char buffer[4096];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        headers.append(buffer, n);
    }
    int status = ::pclose(pipe);
    ::unlink(tmpfile.c_str());

    std::string curl_err;
    if (!err_file.empty()) {
        std::ifstream ef(err_file);
        curl_err.assign(std::istreambuf_iterator<char>(ef), std::istreambuf_iterator<char>());
        ::unlink(err_file.c_str());
        while (!curl_err.empty() && (curl_err.back() == '\n' || curl_err.back() == '\r' || curl_err.back() == ' ')) {
            curl_err.pop_back();
        }
    }

    if (status != 0) {
        result.error = curl_err.empty() ? "curl failed (is curl installed and reachable?)" : ("curl: " + curl_err);
        return result;
    }

    std::string key;
    std::size_t pos = 0;
    while (pos < headers.size()) {
        std::size_t eol = headers.find('\n', pos);
        std::string line = headers.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? headers.size() : eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (toLower(line).rfind("location:", 0) == 0) {
            std::string value = line.substr(9);
            std::size_t start = value.find_first_not_of(" \t");
            if (start != std::string::npos) {
                value = value.substr(start);
            }
            std::size_t slash = value.find_last_of('/');
            key = slash == std::string::npos ? value : value.substr(slash + 1);
        }
    }

    if (key.empty()) {
        result.error = "bytebin did not return a content key";
        return result;
    }
    result.ok = true;
    result.key = key;
    return result;
}

#endif

}  // namespace spark
