extern "C" void spark_gateway_unlinked_import();
extern "C" void spark_gateway_external_import();

extern "C" __attribute__((visibility("default"))) void *provider_binding_anchor()
{
    return reinterpret_cast<void *>(&spark_gateway_unlinked_import);
}

extern "C" __attribute__((visibility("default"))) void provider_lazy_spark()
{
    spark_gateway_external_import();
}
