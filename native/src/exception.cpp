#include <raynium/exception.h>

#include <cstdlib>
#include <exception>

#include <radray/logger.h>

namespace raynium {
void AbortUnexpectedException(std::string_view operation, std::source_location location) noexcept {
    try {
        throw;
    } catch (const std::exception& failure) {
        radray::LogFormatLoc(location, radray::LogLevel::Critical, "{}: unexpected C++ exception: {}", operation, failure.what());
    } catch (...) {
        radray::LogFormatLoc(location, radray::LogLevel::Critical, "{}: unexpected C++ exception", operation);
    }
    std::abort();
}
}  // namespace raynium
