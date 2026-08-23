#include <cassert>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/command/arguments.h"

namespace {

void expectParseError(const std::vector<std::string> &tokens, bool allow_subcommand, const std::string &message)
{
    try {
        spark::Arguments arguments(tokens, allow_subcommand);
        static_cast<void>(arguments);
        assert(false);
    }
    catch (const spark::Arguments::ParseError &error) {
        assert(error.what() == message);
    }
}

}  // namespace

int main()
{
    const spark::Arguments subcommand({"start"}, true);
    assert(subcommand.subCommand() == "start");
    expectParseError({"start"}, false, "Expected flag at position 0 but got 'start' instead!");
    expectParseError({"start", "extra"}, true, "Expected flag at position 1 but got 'extra' instead!");
    expectParseError({"--"}, false, "Expected flag at position 0 but got '--' instead!");

    const spark::Arguments values(
        {"--message", "hello", "world", "--bare", "--tag", "one", "--tag", "one", "--tag", "two"}, false);
    assert(values.stringFlag("MESSAGE") == std::vector<std::string>{"hello world"});
    assert(values.boolFlag("BaRe"));
    assert(values.stringFlag("bare") == std::vector<std::string>{""});
    assert(values.stringFlag("tag") == std::vector<std::string>({"one", "two"}));

    const spark::Arguments quoted(
        spark::Arguments::tokenize(R"(start --thread "Server thread" --thread '^Worker \d+$' --regex)"), true);
    assert(quoted.stringFlag("THREAD") == std::vector<std::string>({"Server thread", R"(^Worker \d+$)"}));
    assert(quoted.boolFlag("REGEX"));

    const spark::Arguments numeric({"--integer", "-42", "--decimal", "-4.5"}, false);
    assert(numeric.intFlag("INTEGER") == 42);
    assert(numeric.doubleFlag("DECIMAL") == 4.5);
    const spark::Arguments invalid({"--integer", "12x", "--decimal", "NaN"}, false);
    assert(!invalid.intFlag("integer"));
    assert(!invalid.doubleFlag("decimal"));
    const spark::Arguments nonfinite({"--decimal", "inf"}, false);
    assert(!nonfinite.doubleFlag("decimal"));

    return 0;
}
