#pragma once

#include <mutex>
#include <string_view>

#include "krypton/updater/updater_seam.hpp"

namespace kr::updater {

class UpdateStateMachine {
 public:
  explicit UpdateStateMachine(IUpdaterObserver* observer = nullptr);

  void SetObserver(IUpdaterObserver* observer) noexcept;
  [[nodiscard]] UpdateState CurrentState() const noexcept;

  // Transition to a new state if valid
  bool TransitionTo(UpdateState next_state);

  // Send progress and error events
  void ReportProgress(const ProgressReport& progress);
  void ReportSecurityAlert(const SecurityAlert& alert);
  void ReportError(std::string_view message);

 private:
  [[nodiscard]] static bool IsValidTransition(UpdateState from, UpdateState to) noexcept;

  mutable std::mutex mutex_;
  IUpdaterObserver* observer_{nullptr};
  UpdateState current_state_{UpdateState::kIdle};
};

}  // namespace kr::updater
