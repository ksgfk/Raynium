#pragma once

#include <source_location>
#include <string_view>

namespace raynium {
// Call only from a catch handler; unexpected failures cannot cross a native boundary.
[[noreturn]] void AbortUnexpectedException(
    std::string_view operation,
    std::source_location location = std::source_location::current()) noexcept;
}  // namespace raynium
