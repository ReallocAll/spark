#include <cassert>
#include <cmath>

#include "core/stats/network_monitor.h"

int main()
{
    {
        spark::DoubleRollingAverage average(5);
        assert(average.samples() == 0);
        assert(average.mean() == 0.0);
        assert(average.max() == 0.0);
        assert(average.min() == 0.0);
        assert(average.median() == 0.0);
        assert(average.percentile95() == 0.0);
    }
    {
        spark::DoubleRollingAverage average(5);
        average.add(10.0);
        average.add(20.0);
        average.add(30.0);
        assert(average.samples() == 3);
        assert(std::abs(average.mean() - 20.0) < 0.001);
        assert(average.min() == 10.0);
        assert(average.max() == 30.0);
        assert(std::abs(average.median() - 20.0) < 0.001);
        assert(std::abs(average.percentile95() - 30.0) < 0.001);
    }
    {
        spark::DoubleRollingAverage average(3);
        average.add(1.0);
        average.add(2.0);
        average.add(3.0);
        average.add(4.0);
        assert(average.samples() == 3);
        assert(std::abs(average.mean() - 3.0) < 0.001);
        assert(average.min() == 2.0);
        assert(average.max() == 4.0);
        assert(std::abs(average.median() - 3.0) < 0.001);
    }
    return 0;
}
