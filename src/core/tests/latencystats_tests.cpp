#include "latencystats.h"
#include <cmath>
#include <cstdio>
#include <limits>

int main()
{
    using namespace ps5cam::latency;
    int failures = 0;
    auto check = [&](bool value, const char* name) {
        if (!value) { ++failures; std::fprintf(stderr, "%s\n", name); }
    };
    check(ElapsedMs(20000, 35000).value_or(-1) == 1.5, "100ns timestamps must become milliseconds");
    check(!ElapsedMs(35000, 20000), "future timestamps must not wrap into a latency");
    check(ElapsedMs(90000, 90000).value_or(-1) == 0, "simultaneous boundaries have zero elapsed time");
    Series ages;
    check(!ages.Summarize(), "missing samples must not look like zero latency");
    for (double value : {15.0, 1.0, 9.0, 3.0}) check(ages.Add(value), "valid sample rejected");
    check(!ages.Add(-1) && !ages.Add(std::numeric_limits<double>::infinity()) &&
          !ages.Add(std::numeric_limits<double>::quiet_NaN()), "invalid measurement accepted");
    const auto result = ages.Summarize();
    check(result && result->count == 4 && result->mean == 7 && result->p50 == 3 &&
          result->p95 == 15 && result->maximum == 15, "known sample distribution misreported");
    check(ages.Summarize()->mean == 7, "reporting must not change the recorded measurements");
    std::printf("latency statistics: %d failures\n", failures);
    return failures ? 1 : 0;
}
