#include <string_view>
#include <windows.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "krypton/updater/updater_engine.hpp"
#include "krypton/updater/updater_seam.hpp"
#include "krypton/updater/windows/winui_installer_window.hpp"

// Standalone interactive UI Preview runner
// Simulates welcome screen, "Unlock the Experience", advanced options, and
// installation progress
int WINAPI wWinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/,
                    PWSTR /*lpCmdLine*/, int /*nCmdShow*/) {
  kr::updater::EngineOptions options;
  options.preview_mode = true;
  options.uninstall_mode =
      std::wstring_view(GetCommandLineW()).find(L"--uninstall") !=
      std::wstring_view::npos;
  kr::updater::UpdaterEngine engine(options);

  auto view = std::make_unique<kr::updater::windows::WinUIInstallerWindow>();
  view->Initialize(&engine);

  // Monitor thread: waits for user to click "Unlock the Experience"
  std::jthread sim_thread([view = view.get(), &engine](std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (engine.CurrentState() != kr::updater::UpdateState::kIdle) {
        break;
      }
    }
    if (stop.stop_requested())
      return;

    // Simulate download & install
    view->OnStateChanged(kr::updater::UpdateState::kChecking);
    view->OnProgress(
        {0, 100ULL * 1024 * 1024, 0.0, 0.0f, "Connecting to update server..."});

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    view->OnStateChanged(kr::updater::UpdateState::kDownloading);

    const uint64_t total_bytes = 85ULL * 1024 * 1024;
    for (int p = 0; p <= 80 && !stop.stop_requested(); ++p) {
      float frac = static_cast<float>(p) / 100.0f;
      uint64_t downloaded = static_cast<uint64_t>(total_bytes * frac);
      view->OnProgress({downloaded, total_bytes, 8.0 * 1024 * 1024, frac,
                        "Downloading component package..."});
      std::this_thread::sleep_for(std::chrono::milliseconds(35));
    }

    if (stop.stop_requested())
      return;
    view->OnStateChanged(kr::updater::UpdateState::kInstalling);

    for (int p = 81; p <= 100 && !stop.stop_requested(); ++p) {
      float frac = static_cast<float>(p) / 100.0f;
      view->OnProgress({total_bytes, total_bytes, 0.0, frac,
                        "Extracting components with 7-Zip..."});
      std::this_thread::sleep_for(std::chrono::milliseconds(45));
    }

    if (!stop.stop_requested()) {
      view->OnStateChanged(kr::updater::UpdateState::kCompleted);
    }
  });

  return view->RunModal();
}
