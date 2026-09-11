extern "C" int fixture_shutdown();

extern "C" int provider_dependency()
{
    return fixture_shutdown();
}
