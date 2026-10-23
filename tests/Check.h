#pragma once

#include <cmath>
#include <cstdio>

inline int failures = 0;

inline void check (bool ok, const char* what)
{
    std::printf ("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (! ok)
        ++failures;
}

inline void checkNear (double value, double expected, double tolerance, const char* what)
{
    const bool ok = std::abs (value - expected) <= tolerance;
    std::printf ("%s %s (%.3f, expected %.3f ± %.3f)\n", ok ? "ok  " : "FAIL", what, value, expected, tolerance);
    if (! ok)
        ++failures;
}
