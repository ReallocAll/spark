class SparkMod final {
public:
    bool load();
    bool enable();
    bool disable();
    bool unload();

private:
    using Executor = ll::thread::ServerThreadExecutor;
    using CallbackState = spark::levilamina::CallbackState;
    using CleanupDeadlineGuard = spark::levilamina::CleanupDeadlineGuard;

    [[nodiscard]] static ll::GamingStatus gamingStatus();
    [[nodiscard]] ll::io::Logger &logger() const noexcept;
    [[nodiscard]] bool registerCommand();
    [[nodiscard]] bool confirmCommandRegistryCleanup();
    [[nodiscard]] bool runAllocatorProbe();
    [[nodiscard]] bool closeResources(bool require_server_thread);
    [[nodiscard]] bool admitRuntimeClose() const;
    void resetSessionState() noexcept;
    void reportException(char const *operation, std::exception_ptr exception) const noexcept;

    friend struct SparkModTestAccess;

    std::weak_ptr<ll::mod::NativeMod> owner_;
    std::weak_ptr<ll::io::Logger> logger_;
    std::shared_ptr<CallbackState> callback_state_;
    std::unique_ptr<CleanupDeadlineGuard> cleanup_guard_;
    std::shared_ptr<spark::levilamina::StartupClock> startup_clock_;
    std::shared_ptr<CommandLifetimeGuard> command_lifetime_;
    std::shared_ptr<CommandContext> command_context_;
    std::shared_ptr<HostSession> host_session_;
    std::shared_ptr<Executor> executor_;
    ll::event::ListenerPtr tick_listener_;
    std::shared_ptr<std::atomic_bool> tick_warning_once_;
    std::atomic_bool enabled_{false};
    bool loaded_ = false;
    bool command_registered_ = false;
    PublicationBoundary publication_boundary_;
};
