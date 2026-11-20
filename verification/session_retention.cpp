#include "edtrace/session_retention.hpp"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
namespace fs = std::filesystem;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "FAIL: %s\n", message);
  std::exit(1);
}

void Write(const fs::path& file) {
  fs::create_directories(file.parent_path());
  std::ofstream out(file);
  out << "fixture";
  out.close();
  Check(static_cast<bool>(out), "create fixture file");
}
}

int wmain(int argc, wchar_t** argv) {
  Check(argc == 2, "owned fixture root required");
  const fs::path root(argv[1]);
  const auto pid = std::to_wstring(GetCurrentProcessId());
  const auto current = pid + L"-1";
  const auto live = pid + L"-18446744073709551615";
  const auto old_reused_pid = root / (pid + L"-2");
  const auto status = L"restoration-status.json";
  Write(root / current / status);
  Write(root / live / status);
  Write(old_reused_pid / status);
  Write(old_reused_pid / L"macos-haptics-status.json");
  Write(old_reused_pid / (L"native-gain-" + pid + L".jsonl"));
  Write(old_reused_pid / (L"onfoot-events-" + pid + L"-4.jsonl"));
  Write(old_reused_pid / (L"authored-trigger-" + pid + L"-5.jsonl"));
  for (unsigned stamp = 1000; stamp < 1080; ++stamp)
    Write(old_reused_pid / (L"authored-trigger-" + pid + L"-" + std::to_wstring(stamp) + L".jsonl"));
  {
    std::ofstream instance(old_reused_pid / L"process-instance.txt", std::ios::binary);
    const std::wstring marker = L"00000000000000000001";
    instance.write(reinterpret_cast<const char*>(marker.data()), marker.size() * sizeof(wchar_t));
  }
  const auto legacy_live = root / (pid + L"-3");
  Write(legacy_live / status);
  {
    std::ofstream partial(legacy_live / L"process-instance.txt",std::ios::binary);
    partial.write(reinterpret_cast<const char*>(L"1"),sizeof(wchar_t));
  }
  const auto unmarked_live = root / (pid + L"-5");
  Write(unmarked_live / status);

  // The highest possible DWORD is never an allocated Windows process ID.
  for (int stamp = 10; stamp <= 15; ++stamp) {
    Write(root / (L"4294967295-" + std::to_wstring(stamp)) / status);
  }
  const auto user_data = root / L"4294967295-10";
  Write(user_data / L"saved.txt");
  Write(user_data / L"nested" / status);
  Write(user_data / L"onfoot-events-4294967295-backup.jsonl");
  const auto foreign_folder = root / L"4294967296-9";
  Write(foreign_folder / status);
  for (int stamp = 100; stamp <= 104; ++stamp)
    Write(root / (L"4294967295-" + std::to_wstring(stamp)) / L"saved.txt");
  const auto empty_folder = root / L"4294967295-200";
  fs::create_directories(empty_folder);

  edtrace::PruneDiagnosticSessions(root.native(), current);

  Check(fs::exists(root / current / status), "current session is preserved");
  Check(fs::exists(root / live / status), "other live session is preserved");
  Check(!fs::exists(old_reused_pid), "old logs are removed after PID reuse");
  Check(fs::exists(legacy_live / status) && fs::exists(unmarked_live / status),
        "a live PID with absent or partial identity is preserved despite an earlier session timestamp");
  Check(!fs::exists(root / L"4294967295-11"), "old inactive session is removed");
  for (int stamp = 12; stamp <= 15; ++stamp) {
    Check(fs::exists(root / (L"4294967295-" + std::to_wstring(stamp)) / status),
          "four newest inactive sessions are preserved");
  }
  Check(!fs::exists(user_data / status), "old mod log is removed beside user files");
  Check(fs::exists(user_data / L"saved.txt") && fs::exists(user_data / L"nested" / status) &&
        fs::exists(user_data / L"onfoot-events-4294967295-backup.jsonl"), "unrecognized files and subdirectories are preserved");
  Check(fs::exists(foreign_folder / status), "out-of-range PID is ignored");
  Check(fs::exists(empty_folder), "an unrelated empty PID-shaped folder is preserved");
  const auto rollback_live = root / (pid + L"-4");
  Write(rollback_live / status);
  fs::copy_file(root / current / L"process-instance.txt", rollback_live / L"process-instance.txt");
  edtrace::PruneDiagnosticSessions(root.native(), current);
  Check(fs::exists(rollback_live / status), "a live process instance survives a session clock rollback");
  return 0;
}
