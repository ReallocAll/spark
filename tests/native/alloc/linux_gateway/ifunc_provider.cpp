extern "C" void *spark_fixture_ifunc_target();

extern "C" void (*spark_gateway_ifunc_resolver())()
{
    return reinterpret_cast<void (*)()>(spark_fixture_ifunc_target());
}

extern "C" void spark_gateway_external_import() __attribute__((ifunc("spark_gateway_ifunc_resolver")));
