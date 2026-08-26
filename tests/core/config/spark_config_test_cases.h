#ifndef SPARK_TESTS_CORE_CONFIG_SPARK_CONFIG_TEST_CASES_H
#define SPARK_TESTS_CORE_CONFIG_SPARK_CONFIG_TEST_CASES_H

namespace spark::config_test {

void testDefaults();
void testTomlValidOverride();
void testTomlInvalid();
void testTomlWrongType();
void testBootstrapPreservesInvalidFile();
void testValidation();
void testEndpointValidation();
void testTomlUnknownField();
void testTomlPartial();
void testSaveCreatesToml();
void testSaveAndReload();
void testEmptyToml();
void testTomlWithComments();
void testBoundedAndTrailingInput();
void testEnvironmentOverrides();
void testEnvironmentMissingAndExactNames();
void testEnvironmentInvalidIntegerFallback();
void testEnvironmentBooleanParsing();
void testEnvironmentValidationIsAtomic();
void testEnvironmentLoadOrCreateDoesNotPersist();

}  // namespace spark::config_test

#endif  // SPARK_TESTS_CORE_CONFIG_SPARK_CONFIG_TEST_CASES_H
