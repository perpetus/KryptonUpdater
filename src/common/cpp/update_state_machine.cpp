#include "krypton/updater/update_state_machine.hpp"

namespace kr::updater {

UpdateStateMachine::UpdateStateMachine(IUpdaterObserver *observer)
    : observer_(observer) {}

void UpdateStateMachine::SetObserver(IUpdaterObserver *observer) noexcept {
  std::lock_guard lock(mutex_);
  observer_ = observer;
}

UpdateState UpdateStateMachine::CurrentState() const noexcept {
  std::lock_guard lock(mutex_);
  return current_state_;
}

bool UpdateStateMachine::IsValidTransition(UpdateState from,
                                           UpdateState to) noexcept {
  if (to == UpdateState::kError)
    return true; // Can enter error from any state

  switch (from) {
  case UpdateState::kIdle:
    return to == UpdateState::kChecking || to == UpdateState::kVerifying ||
           to == UpdateState::kExtracting || to == UpdateState::kInstalling;
  case UpdateState::kChecking:
    return to == UpdateState::kUpToDate || to == UpdateState::kUpdateAvailable;
  case UpdateState::kUpToDate:
    return to == UpdateState::kChecking || to == UpdateState::kIdle;
  case UpdateState::kUpdateAvailable:
    return to == UpdateState::kDownloading || to == UpdateState::kIdle;
  case UpdateState::kDownloading:
    return to == UpdateState::kVerifying;
  case UpdateState::kVerifying:
    return to == UpdateState::kExtracting || to == UpdateState::kInstalling;
  case UpdateState::kExtracting:
    return to == UpdateState::kInstalling;
  case UpdateState::kInstalling:
    return to == UpdateState::kReadyToRestart || to == UpdateState::kCompleted;
  case UpdateState::kReadyToRestart:
    return to == UpdateState::kCompleted || to == UpdateState::kIdle;
  case UpdateState::kCompleted:
    return to == UpdateState::kIdle;
  case UpdateState::kError:
    return to == UpdateState::kIdle || to == UpdateState::kChecking;
  }
  return false;
}

bool UpdateStateMachine::TransitionTo(UpdateState next_state) {
  IUpdaterObserver *obs = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (!IsValidTransition(current_state_, next_state)) {
      return false;
    }
    current_state_ = next_state;
    obs = observer_;
  }

  if (obs) {
    obs->OnStateChanged(next_state);
  }
  return true;
}

void UpdateStateMachine::ReportProgress(const ProgressReport &progress) {
  IUpdaterObserver *obs = nullptr;
  {
    std::lock_guard lock(mutex_);
    obs = observer_;
  }
  if (obs) {
    obs->OnProgress(progress);
  }
}

void UpdateStateMachine::ReportSecurityAlert(const SecurityAlert &alert) {
  IUpdaterObserver *obs = nullptr;
  {
    std::lock_guard lock(mutex_);
    obs = observer_;
  }
  if (obs) {
    obs->OnSecurityAlert(alert);
  }
}

void UpdateStateMachine::ReportError(std::string_view message) {
  IUpdaterObserver *obs = nullptr;
  {
    std::lock_guard lock(mutex_);
    current_state_ = UpdateState::kError;
    obs = observer_;
  }
  if (obs) {
    obs->OnError(message);
    obs->OnStateChanged(UpdateState::kError);
  }
}

} // namespace kr::updater
