namespace {
thread_local volatile unsigned Value = 7;
unsigned Initialized = 0;

__attribute__((constructor)) void initialize()
{
    Initialized = 1;
}
}  // namespace

extern "C" unsigned sparkDependencyFixtureValue()
{
    return Value + Initialized;
}

#if defined(SPARK_DEPENDENCY_ROOT)
int main()
{
    return sparkDependencyFixtureValue() == 8 ? 0 : 1;
}
#endif
