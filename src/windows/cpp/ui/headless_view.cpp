#include "krypton/updater/windows/headless_view.hpp"
#include "krypton/updater/updater_engine.hpp"

#include <iostream>

namespace kr::updater::windows {

HeadlessView::HeadlessView(bool verbose) : verbose_(verbose) {}

void HeadlessView::Initialize(UpdaterEngine* engine) {
  engine_ = engine;
  if (engine_) {
    engine_->SetObserver(this);
  }
}

int HeadlessView::RunModal() {
  if (!engine_) return 1;
  return engine_->Execute();
}

void HeadlessView::OnStateChanged(UpdateState new_state) {
  last_state_ = new_state;
  if (verbose_) {
    std::cout << "[KryptonUpdater] State: " << UpdateStateToString(new_state) << "\n";
  }
}

void HeadlessView::OnProgress(const ProgressReport& progress) {
  if (verbose_) {
    std::cout << "\r[KryptonUpdater] Progress: " << static_cast<int>(progress.fraction * 100.0f)
              << "% - " << progress.status_detail << "          " << std::flush;
  }
}

void HeadlessView::OnSecurityAlert(const SecurityAlert& alert) {
  std::cerr << "[KryptonUpdater] Security Alert: " << alert.reason << " (" << alert.target_file << ")\n";
}

void HeadlessView::OnError(std::string_view message) {
  std::cerr << "[KryptonUpdater] Error: " << message << "\n";
  exit_code_ = 1;
}

}  // namespace kr::updater::windows
