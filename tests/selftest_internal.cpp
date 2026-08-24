#include "selftest_internal.h"

#include <cmath>

namespace spark::selftest {

namespace {

volatile double sink = 0.0;

double hotInner(int n)
{
    double result = 0.0;
    for (int i = 0; i < n * 1000; ++i) {
        result += std::sin(i * 0.5) * std::cos(i * 0.25);
    }
    return result;
}

void hotMiddle(int rounds)
{
    for (int i = 0; i < rounds; ++i) {
        sink += hotInner(40);
    }
}

}  // namespace

void hotOuter()
{
    hotMiddle(20);
}

}  // namespace spark::selftest
