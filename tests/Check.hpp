#pragma once

#include <cstdio>
#include <cstdlib>

// Release-safe assertion; assert() compiles out under NDEBUG
#define CHECK(expr)                                                                       \
    do {                                                                                  \
        if (!(expr)) {                                                                    \
            std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
            std::abort();                                                                 \
        }                                                                                 \
    } while (0)
