#pragma once
#include <cstdlib>
#include <iostream>

// Unlike assert(), test expectations must execute in Release builds too.
#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; \
    std::exit(EXIT_FAILURE); \
} } while (false)
