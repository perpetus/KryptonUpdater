#pragma once

#include "krypton/updater/updater_seam.hpp"

#include <memory>

namespace kr::updater::windows {

class WinUIInstallerWindow : public IUpdaterView {
public:
  WinUIInstallerWindow();
  ~WinUIInstallerWindow() override;

  void Initialize(class UpdaterEngine *engine) override;
  int RunModal() override;
  void SetResumed(bool resumed);

  void OnStateChanged(UpdateState new_state) override;
  void OnProgress(const ProgressReport &progress) override;
  void OnSecurityAlert(const SecurityAlert &alert) override;
  void OnError(std::string_view message) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace kr::updater::windows
