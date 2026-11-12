#pragma once
#include <radray/logger.h>
#include <cstdlib>
#include <exception>
#include <source_location>
#include <string_view>

namespace raynium {
// Call only from a catch handler; unexpected failures cannot cross a native boundary.
[[noreturn]] inline void AbortUnexpectedException(std::string_view operation,
    std::source_location location=std::source_location::current()) noexcept {
    try {
        throw;
    } catch(const std::exception& failure) {
        radray::LogFormatLoc(location,radray::LogLevel::Critical,"{}: unexpected C++ exception: {}",operation,failure.what());
    } catch(...) {
        radray::LogFormatLoc(location,radray::LogLevel::Critical,"{}: unexpected C++ exception",operation);
    }
    std::abort();
}
}
