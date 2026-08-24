#include "symbol_guess_windows_test_support.h"

#ifdef _WIN32

namespace spark::symbol_guess::windows_test {

void addClass(PeFixture &fixture, std::uint32_t base, std::string_view name, std::uint32_t table,
              std::initializer_list<std::uint32_t> targets, std::uint32_t offset)
{
    fixture.typeAndCol(base, base + 0x100, base + 0x120, base + 0x140, base + 0x180, name, offset);
    fixture.vtable(table, base + 0x180, targets);
}

}  // namespace spark::symbol_guess::windows_test

#endif
