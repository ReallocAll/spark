bool SparkMod::disable()
{
    if (!enabled_.load(std::memory_order_acquire)) {
        return true;
    }
    try {
        return closeResources(gamingStatus() != ll::GamingStatus::Stopping);
    }
    catch (...) {
        CleanupDeadlineGuard::terminateOnTimeout();
    }
}
