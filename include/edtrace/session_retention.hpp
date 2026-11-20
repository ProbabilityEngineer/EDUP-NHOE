#pragma once

#include <string>
#include <string_view>

namespace edtrace {
void PruneDiagnosticSessions(const std::wstring& root, std::wstring_view current_session) noexcept;
}
