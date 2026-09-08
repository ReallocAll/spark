import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def method_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


class PluginLifecycleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "src" / "plugin.cpp").read_text(encoding="utf-8")
        cls.enable_body = method_body(cls.source, "void onEnable() override", "void onDisable() override")
        cls.disable_body = method_body(cls.source, "void onDisable() override", "bool onCommand(")

    def test_disable_destroys_application_after_final_global_provider_clear(self):
        provider_clear = "spark::setGlobalPythonStackProvider(nullptr);"
        application_reset = "app_.reset();"

        self.assertEqual(self.disable_body.count(provider_clear), 1)
        self.assertEqual(self.disable_body.count(application_reset), 1)
        self.assertRegex(self.disable_body, re.escape(provider_clear) + r"\s*\n\s*" + re.escape(application_reset))
        self.assertNotIn(application_reset, self.enable_body)

    def test_disable_failure_guards_precede_application_reset(self):
        application_reset = self.disable_body.index("app_.reset();")
        aborts = [match.start() for match in re.finditer(r"std::abort\(\);", self.disable_body)]

        self.assertEqual(len(aborts), 2)
        self.assertTrue(all(abort < application_reset for abort in aborts))

    def test_disable_preserves_shutdown_order(self):
        ordered_steps = [
            "metrics_->shutdown();",
            "papi_integration_.disable(*this);",
            "app_->shutdown(application_shutdown_error)",
            "endPythonSession();",
            "getServer().getScheduler().cancelTasks(*this);",
            "app_->shutdownProfilerBackend(shutdown_error)",
            "spark::setGlobalPythonStackProvider(nullptr);",
            "app_.reset();",
        ]
        positions = [self.disable_body.index(step) for step in ordered_steps]
        self.assertEqual(positions, sorted(positions))


if __name__ == "__main__":
    unittest.main()
