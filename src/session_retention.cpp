#include "edtrace/session_retention.hpp"

#include <windows.h>
#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>
#include <array>

namespace edtrace {
namespace {
constexpr std::size_t retained_inactive_sessions = 4;
constexpr DWORD special_entry = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
constexpr wchar_t instance_file[] = L"process-instance.txt";
using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
using Search = std::unique_ptr<void, decltype(&FindClose)>;

bool ReadNumber(std::wstring_view text, ULONGLONG& number) {
  if (text.empty()) return false;
  number = 0;
  for (const auto character : text) {
    if (character < L'0' || character > L'9') return false;
    const auto digit = static_cast<unsigned>(character - L'0');
    if (number > (std::numeric_limits<ULONGLONG>::max() - digit) / 10) return false;
    number = number * 10 + digit;
  }
  return number != 0;
}

struct Session {
  std::wstring name;
  DWORD pid;
  ULONGLONG started;
  Handle directory;
};

ULONGLONG ProcessInstance(HANDLE process) {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
  return (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

ULONGLONG ReadInstance(const std::wstring& path) {
  Handle file(CreateFileW((path + L"\\" + instance_file).c_str(), GENERIC_READ,
                         FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr), CloseHandle);
  if (file.get() == INVALID_HANDLE_VALUE) { file.release(); return 0; }
  BY_HANDLE_FILE_INFORMATION info{};
  std::array<wchar_t, 21> text{};
  DWORD read = 0;
  if (!GetFileInformationByHandle(file.get(), &info) || (info.dwFileAttributes & special_entry) ||
      !ReadFile(file.get(), text.data(), sizeof(text), &read, nullptr) ||
      read != 20 * sizeof(wchar_t)) return 0;
  ULONGLONG result = 0;
  return ReadNumber({text.data(), read / sizeof(wchar_t)}, result) ? result : 0;
}

void WriteInstance(const std::wstring& path) {
  const auto created = ProcessInstance(GetCurrentProcess());
  if (!created) return;
  auto text = std::to_wstring(created);
  text.insert(0, 20 - text.size(), L'0');
  Handle file(CreateFileW((path + L"\\" + instance_file).c_str(), GENERIC_WRITE,
                         FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr), CloseHandle);
  if (file.get() == INVALID_HANDLE_VALUE) { file.release(); return; }
  DWORD written = 0;
  const auto size = static_cast<DWORD>(text.size() * sizeof(wchar_t));
  if (!WriteFile(file.get(), text.data(), size, &written, nullptr) || written != size) {
    file.reset();
    DeleteFileW((path + L"\\" + instance_file).c_str());
  }
}

bool IsInactive(const Session& session, const std::wstring& path) {
  const auto raw = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, session.pid);
  if (!raw) return GetLastError() == ERROR_INVALID_PARAMETER;
  const Handle process(raw, CloseHandle);
  if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) return true;

  const auto recorded = ReadInstance(path);
  const auto actual = ProcessInstance(process.get());
  // A process creation value is stable for its lifetime. A session's wall
  // clock timestamp cannot establish PID reuse after a clock correction.
  return recorded && actual && recorded != actual;
}

bool IsSessionLog(std::wstring_view name, const std::wstring& pid) {
  if (name == L"restoration-status.json" || name == L"macos-haptics-status.json" ||
      name == L"native-gain-" + pid + L".jsonl") return true;

  for (const auto prefix : {L"onfoot-events-", L"authored-trigger-"}) {
    const std::wstring stem = prefix + pid + L"-";
    constexpr std::wstring_view suffix = L".jsonl";
    if (name.size() <= stem.size() + suffix.size() || name.substr(0, stem.size()) != stem ||
        name.substr(name.size() - suffix.size()) != suffix) continue;
    ULONGLONG stamp;
    return ReadNumber(name.substr(stem.size(), name.size() - stem.size() - suffix.size()), stamp);
  }
  return false;
}

Handle OpenDirectory(const std::wstring& path, bool remove = false) {
  // Pin the directory while using paths below it. A redirected directory is
  // never a cleanup target.
  const auto raw = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | (remove ? DELETE : 0),
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  Handle directory(raw == INVALID_HANDLE_VALUE ? nullptr : raw, CloseHandle);
  BY_HANDLE_FILE_INFORMATION info{};
  if (directory && (!GetFileInformationByHandle(directory.get(), &info) ||
                    (info.dwFileAttributes & special_entry) != FILE_ATTRIBUTE_DIRECTORY)) directory.reset();
  return directory;
}

template <typename Visit>
bool EachRegularFile(HANDLE directory, Visit visit) {
  alignas(FILE_ID_BOTH_DIR_INFO) std::array<BYTE,4096> buffer{};
  auto kind = FileIdBothDirectoryRestartInfo;
  while (GetFileInformationByHandleEx(directory,kind,buffer.data(),static_cast<DWORD>(buffer.size()))) {
    kind = FileIdBothDirectoryInfo;
    std::size_t offset = 0;
    for (;;) {
      constexpr auto head = offsetof(FILE_ID_BOTH_DIR_INFO,FileName);
      if (offset > buffer.size()-head) return false;
      const auto& entry = *reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data()+offset);
      if (entry.FileNameLength % sizeof(wchar_t) || entry.FileNameLength > buffer.size()-offset-head) return false;
      if (!(entry.FileAttributes & special_entry) &&
          !visit(std::wstring_view(entry.FileName,entry.FileNameLength/sizeof(wchar_t)))) return true;
      if (!entry.NextEntryOffset) break;
      if (entry.NextEntryOffset < head+entry.FileNameLength ||
          entry.NextEntryOffset > buffer.size()-offset ||
          entry.NextEntryOffset % alignof(FILE_ID_BOTH_DIR_INFO)) return false;
      offset += entry.NextEntryOffset;
    }
  }
  return GetLastError() == ERROR_NO_MORE_FILES;
}

bool HasSessionLog(HANDLE directory, DWORD pid) {
  bool found = false;
  EachRegularFile(directory,[&](std::wstring_view name) {
    found = IsSessionLog(name,std::to_wstring(pid));
    return !found;
  });
  return found;
}

void RemoveSession(const std::wstring& path, Session& session) {
  if (!session.directory || !HasSessionLog(session.directory.get(),session.pid) || !IsInactive(session,path)) return;
  const auto pid_text = std::to_wstring(session.pid);
  std::vector<std::wstring> logs;
  if (!EachRegularFile(session.directory.get(),[&](std::wstring_view name) {
    if (IsSessionLog(name,pid_text) || name == instance_file)
      logs.emplace_back(name);
    return true;
  })) return;
  for (const auto& name : logs) DeleteFileW((path + L"\\" + name).c_str());
  // Delete the pinned instance. A path-based removal after closing its handle
  // could remove a directory swapped into the same name.
  FILE_DISPOSITION_INFO disposition{TRUE};
  SetFileInformationByHandle(session.directory.get(), FileDispositionInfo, &disposition, sizeof(disposition));
}
}  // namespace

void PruneDiagnosticSessions(const std::wstring& root, std::wstring_view current_session) noexcept {
  try {
    const auto directory = OpenDirectory(root);
    if (!directory) return;
    {
      const auto current_path = root + L"\\" + std::wstring(current_session);
      const auto current = OpenDirectory(current_path);
      if (current) WriteInstance(current_path);
    }
    WIN32_FIND_DATAW entry{};
    const auto raw = FindFirstFileW((root + L"\\*").c_str(), &entry);
    if (raw == INVALID_HANDLE_VALUE) return;
    std::vector<Session> sessions;
    {
      const Search search(raw, FindClose);
      do {
        if ((entry.dwFileAttributes & special_entry) != FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring_view name(entry.cFileName);
        if (name == current_session) continue;
        const auto divider = name.find(L'-');
        ULONGLONG pid, started;
        if (divider == name.npos || !ReadNumber(name.substr(0, divider), pid) ||
            pid > std::numeric_limits<DWORD>::max() || !ReadNumber(name.substr(divider + 1), started)) continue;
        const auto path = root + L"\\" + std::wstring(name);
        Session session{std::wstring(name), static_cast<DWORD>(pid), started, OpenDirectory(path,true)};
        if (session.directory && HasSessionLog(session.directory.get(), session.pid) && IsInactive(session, path))
          sessions.push_back(std::move(session));
      } while (FindNextFileW(search.get(), &entry));
      if (GetLastError() != ERROR_NO_MORE_FILES) return;
    }
    std::sort(sessions.begin(), sessions.end(), [](const Session& a, const Session& b) {
      return a.started != b.started ? a.started > b.started : a.name > b.name;
    });
    for (std::size_t i = retained_inactive_sessions; i < sessions.size(); ++i) {
      RemoveSession(root + L"\\" + sessions[i].name, sessions[i]);
    }
  } catch (...) {
    // Diagnostics cleanup must never prevent haptics startup.
  }
}
}  // namespace edtrace
