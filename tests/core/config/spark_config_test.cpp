#include <cstdio>

#include "spark_config_test_cases.h"

int main()
{
    using namespace spark::config_test;  // NOLINT(google-build-using-namespace)

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("Running SparkConfig tests...\n");
    testDefaults();
    testTomlValidOverride();
    testTomlInvalid();
    testTomlWrongType();
    testBootstrapPreservesInvalidFile();
    testValidation();
    testEndpointValidation();
    testTomlUnknownField();
    testTomlPartial();
    testSaveCreatesToml();
    testSaveAndReload();
    testEmptyToml();
    testTomlWithComments();
    testBoundedAndTrailingInput();
    std::printf("All SparkConfig tests passed!\n");
    return 0;
}
