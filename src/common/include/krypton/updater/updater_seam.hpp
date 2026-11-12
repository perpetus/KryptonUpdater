#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace kr::updater {

enum class UpdateState {
  kIdle,
  kChecking,
  kUpToDate,
  kUpdateAvailable,
  kDownloading,
  kVerifying,
  kExtracting,
  kInstalling,
  kReadyToRestart,
  kCompleted,
  kError
};

[[nodiscard]] constexpr std::string_view UpdateStateToString(UpdateState state) noexcept {
  switch (state) {
    case UpdateState::kIdle: return "Idle";
    case UpdateState::kChecking: return "Checking for updates...";
    case UpdateState::kUpToDate: return "Krypton is up to date";
    case UpdateState::kUpdateAvailable: return "Update available";
    case UpdateState::kDownloading: return "Downloading update...";
    case UpdateState::kVerifying: return "Verifying package integrity...";
    case UpdateState::kExtracting: return "Extracting files...";
    case UpdateState::kInstalling: return "Installing...";
    case UpdateState::kReadyToRestart: return "Ready to restart";
    case UpdateState::kCompleted: return "Installation completed";
    case UpdateState::kError: return "Error occurred";
  }
  return "Unknown";
}

struct ProgressReport {
  std::uint64_t bytes_processed = 0;
  std::uint64_t total_bytes = 0;
  double bytes_per_second = 0.0;
  float fraction = 0.0f; // 0.0 to 1.0
  std::string status_detail;
};

struct SecurityAlert {
  std::string reason;
  std::string target_file;
  bool is_critical = true;
};

class IUpdaterObserver {
 public:
  virtual ~IUpdaterObserver() = default;
  virtual void OnStateChanged(UpdateState new_state) = 0;
  virtual void OnProgress(const ProgressReport& progress) = 0;
  virtual void OnSecurityAlert(const SecurityAlert& alert) = 0;
  virtual void OnError(std::string_view message) = 0;
};

class IUpdaterView : public IUpdaterObserver {
 public:
  virtual void Initialize(class UpdaterEngine* engine) = 0;
  virtual int RunModal() = 0;
};

}  // namespace kr::updater
