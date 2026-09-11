#include <dlfcn.h>

extern "C" __attribute__((visibility("default"))) void *provider_lookup_spark()
{
    return ::dlsym(RTLD_DEFAULT, "spark_gateway_unlinked_import");
}
