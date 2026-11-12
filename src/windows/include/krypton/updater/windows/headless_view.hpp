#pragma once

#include <iostream>
#include <string>

#include "krypton/updater/updater_seam.hpp"

namespace kr::updater::windows {

class HeadlessView : public IUpdaterView {
 public:
  HeadlessView(bool verbose = false);

  void Initialize(class UpdaterEngine* engine) override;
  int RunModal() override;

  void OnStateChanged(UpdateState new_state) override;
  void OnProgress(const ProgressReport& progress) override;
  void OnSecurityAlert(const SecurityAlert& alert) override;
  void OnError(std::string_view message) override;

  [[nodiscard]] UpdateState FinalState() const noexcept { return last_state_; }

 private:
  bool verbose_{false};
  UpdateState last_state_{UpdateState::kIdle};
  int exit_code_{0};
  class UpdaterEngine* engine_{nullptr};
};

}  // namespace kr::updater::windows
