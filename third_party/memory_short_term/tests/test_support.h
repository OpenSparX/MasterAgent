#pragma once

#include <cstdlib>
#include <iostream>
#include <string>

namespace vehicle_memory::test_support {

inline void Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "EXPECT FAILED: " << message << std::endl;
    std::exit(1);
  }
}

}  // namespace vehicle_memory::test_support
