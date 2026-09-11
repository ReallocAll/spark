extern "C" void spark_fixture_constructor_gate();

namespace {
__attribute__((constructor)) void blockLoader()
{
    spark_fixture_constructor_gate();
}
}  // namespace
