#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "krypton/updater/windows/locked_file.hpp"

namespace kr::updater::windows {

class PayloadRunner {
 public:
  // Runs an MSI installer silently (msiexec.exe /i ... /qn)
  [[nodiscard]] static bool RunMsi(
      const std::filesystem::path& msi_path,
      bool per_user = true,
      std::uint32_t* out_exit_code = nullptr);

  // Runs an external EXE installer silently
  [[nodiscard]] static bool
  RunExe(const std::filesystem::path &exe_path,
         const std::wstring &arguments = L"/SILENT /NORESTART",
         std::uint32_t *out_exit_code = nullptr,
         ScopedFileHandle verification_lock = {});
};

}  // namespace kr::updater::windows
