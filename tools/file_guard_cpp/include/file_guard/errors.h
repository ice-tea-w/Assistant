#pragma once

#include <stdexcept>
#include <string>

namespace file_guard {

enum class ErrorCode : int {
    InvalidArgument = 1,
    Password = 3,
    Corrupt = 4,
    Integrity = 5,
    Io = 6,
    Platform = 7,
};

class SecurityError : public std::runtime_error {
public:
    SecurityError(ErrorCode code, const std::string &message)
        : std::runtime_error(message), m_code(code) {}
    ErrorCode code() const { return m_code; }

private:
    ErrorCode m_code;
};

} // namespace file_guard
