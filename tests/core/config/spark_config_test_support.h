#ifndef SPARK_TESTS_CORE_CONFIG_SPARK_CONFIG_TEST_SUPPORT_H
#define SPARK_TESTS_CORE_CONFIG_SPARK_CONFIG_TEST_SUPPORT_H

#include <filesystem>
#include <fstream>
#include <string>

namespace spark::config_test {

inline std::filesystem::path tempDir()
{
    auto dir = std::filesystem::temp_directory_path() / "spark_config_tests";
    std::filesystem::create_directories(dir);
    return dir;
}

inline void writeFile(const std::filesystem::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary);
    out << content;
    out.close();
}

inline void cleanup(const std::filesystem::path &path)
{
    std::filesystem::remove(path);
}

}  // namespace spark::config_test

#endif  // SPARK_TESTS_CORE_CONFIG_SPARK_CONFIG_TEST_SUPPORT_H
