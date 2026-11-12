#pragma once

#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>

#include "krypton/updater/archive_extractor.hpp"
#include "krypton/updater/manifest_verifier.hpp"
#include "krypton/updater/thread_pool.hpp"
#include "krypton/updater/update_state_machine.hpp"
#include "krypton/updater/updater_seam.hpp"

namespace kr::updater {

struct EngineOptions {
  std::string repo_owner = "perpetus";
  std::string repo_name = "krypton-windows";
  std::filesystem::path install_root;
  std::filesystem::path custom_package_path;
  bool force_offline = false;
  bool uninstall_mode = false;
  bool preview_mode = false;
  bool verify_only_for_testing = false;
  bool silent = false;
  bool check_only = false;
  bool skip_shell_registration = false;
  // The payload verification harness must not modify machine prerequisites.
  // This option is deliberately not exposed by the installer command line.
  bool skip_prerequisites_for_testing = false;
  bool create_desktop_shortcut = true;
  bool create_start_menu_shortcut = true;
};

class UpdaterEngine {
public:
  explicit UpdaterEngine(EngineOptions options,
                         IUpdaterObserver *observer = nullptr);
  ~UpdaterEngine();

  void SetObserver(IUpdaterObserver *observer);
  [[nodiscard]] UpdateState CurrentState() const noexcept;
  [[nodiscard]] const EngineOptions &Options() const noexcept {
    return options_;
  }
  [[nodiscard]] EngineOptions &MutableOptions() noexcept { return options_; }

  // Starts the update/install pipeline (synchronous or on worker thread)
  int Execute();

  [[nodiscard]] const std::filesystem::path &
  InstalledExecutable() const noexcept {
    return installed_executable_;
  }

  // Requests cancellation of current operation
  void Cancel();

private:
  int ExecuteOffline(const std::filesystem::path &payload_path,
                     std::string_view embedded_manifest = {});
  int ExecuteOnline();
  int ExecuteUninstall();

  EngineOptions options_;
  ThreadPool thread_pool_;
  UpdateStateMachine state_machine_;
  std::atomic<bool> cancelled_{false};
  std::stop_source stop_source_;
  std::filesystem::path installed_executable_;
};

} // namespace kr::updater
