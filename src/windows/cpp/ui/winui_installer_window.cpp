#include "krypton/updater/windows/winui_installer_window.hpp"
#include "krypton/updater/updater_engine.hpp"
#include "krypton/updater/windows/os_capability.hpp"
#include "krypton/updater/windows/resource_ids.h"

// GDI+ requires the Win32 and COM declarations before its headers.
// clang-format off
#include <windows.h>

#include <objbase.h>
#include <unknwn.h>

#include <dwmapi.h>
#include <gdiplus.h>
#include <inspectable.h>
#include <shcore.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
// clang-format on
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.h>
#include <winrt/base.h>

#ifndef __IXamlSourceTransparency_INTERFACE_DEFINED__
#define __IXamlSourceTransparency_INTERFACE_DEFINED__
MIDL_INTERFACE("06636c29-5a17-458d-8ea2-2422d997a922")
IXamlSourceTransparency : public IInspectable {
public:
  virtual HRESULT STDMETHODCALLTYPE get_IsBackgroundTransparent(boolean *
                                                                value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_IsBackgroundTransparent(
      boolean value) = 0;
};
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "shlwapi.lib")

namespace xaml = winrt::Windows::UI::Xaml;
namespace controls = winrt::Windows::UI::Xaml::Controls;
namespace hosting = winrt::Windows::UI::Xaml::Hosting;
namespace media = winrt::Windows::UI::Xaml::Media;
namespace text = winrt::Windows::UI::Text;
namespace streams = winrt::Windows::Storage::Streams;
namespace shapes = winrt::Windows::UI::Xaml::Shapes;

namespace kr::updater::windows {

namespace {

void AddRoundedRect(Gdiplus::GraphicsPath &path, const Gdiplus::RectF &rect,
                    float radius) {
  float diameter = radius * 2.0f;
  float right = rect.X + rect.Width;
  float bottom = rect.Y + rect.Height;
  path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
  path.AddArc(right - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
  path.AddArc(right - diameter, bottom - diameter, diameter, diameter, 0.0f,
              90.0f);
  path.AddArc(rect.X, bottom - diameter, diameter, diameter, 90.0f, 90.0f);
  path.CloseFigure();
}

std::wstring PickFolderDialog(HWND parent, const std::wstring &current_path) {
  IFileOpenDialog *pfd = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
    return current_path;
  }
  DWORD dwOptions = 0;
  pfd->GetOptions(&dwOptions);
  pfd->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  pfd->SetTitle(L"Select Krypton Installation Location");

  if (!current_path.empty()) {
    IShellItem *psi = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(current_path.c_str(), nullptr,
                                              IID_PPV_ARGS(&psi)))) {
      pfd->SetFolder(psi);
      psi->Release();
    }
  }

  std::wstring selected = current_path;
  if (SUCCEEDED(pfd->Show(parent))) {
    IShellItem *psiResult = nullptr;
    if (SUCCEEDED(pfd->GetResult(&psiResult))) {
      PWSTR pszPath = nullptr;
      if (SUCCEEDED(psiResult->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
        selected = pszPath;
        CoTaskMemFree(pszPath);
      }
      psiResult->Release();
    }
  }
  pfd->Release();
  return selected;
}

std::wstring GetDynamicStageText(kr::updater::UpdateState state, float percent,
                                 bool is_resumed = false) {
  if (is_resumed) {
    return L"Right back where we left off...";
  }

  uint32_t p =
      static_cast<uint32_t>(std::clamp(percent * 100.0f, 0.0f, 100.0f));
  switch (state) {
  case kr::updater::UpdateState::kChecking:
    return L"A fresh start begins here...";
  case kr::updater::UpdateState::kDownloading:
  case kr::updater::UpdateState::kExtracting:
  case kr::updater::UpdateState::kInstalling:
  case kr::updater::UpdateState::kCompleted:
  default:
    if (p == 0)
      return L"A fresh start begins here...";
    if (p <= 24)
      return L"Bringing Krypton to you...";
    if (p <= 49)
      return L"Your next browsing experience is taking shape...";
    if (p <= 74)
      return L"Getting everything into place...";
    if (p <= 79)
      return L"Almost ready to explore...";
    if (p <= 99)
      return L"Setting the stage for what's next...";
    return L"The internet looks better from here.";
  case kr::updater::UpdateState::kError:
    return L"Installation encountered an issue.";
  }
}

bool ApplyMicaBackdrop(HWND hwnd) noexcept {
  // Keep every native surface light and enable in-box XAML host backdrop
  // brushes.
  const BOOL host_backdrop = TRUE;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &host_backdrop,
                        sizeof(host_backdrop));
  // 1. Immersive Dark Mode for window frame and DWM
  const BOOL dark = FALSE;
  if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)))) {
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
  }

  // 2. Modern rounded corners (Windows 11)
  DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners,
                        sizeof(corners));

  // Keep a system backdrop behind transparent client pixels for the software
  // fallback. XAML uses one host Acrylic brush for both the caption and body.
  BOOL composition_enabled = FALSE;
  const MARGINS full_frame{-1, -1, -1, -1};
  if (SUCCEEDED(DwmIsCompositionEnabled(&composition_enabled)) &&
      composition_enabled &&
      SUCCEEDED(DwmExtendFrameIntoClientArea(hwnd, &full_frame))) {
    for (const auto backdrop_type :
         {DWMSBT_TRANSIENTWINDOW, DWMSBT_MAINWINDOW}) {
      if (SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                          &backdrop_type,
                                          sizeof(backdrop_type)))) {
        return true;
      }
    }

    // Windows 11 21H2 (Build 22000): DWMWA_MICA_EFFECT = 1029.
    const BOOL mica_effect = TRUE;
    if (SUCCEEDED(DwmSetWindowAttribute(hwnd, 1029, &mica_effect,
                                        sizeof(mica_effect)))) {
      return true;
    }
  }

  const auto no_backdrop = DWMSBT_NONE;
  DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &no_backdrop,
                        sizeof(no_backdrop));
  const BOOL no_mica = FALSE;
  DwmSetWindowAttribute(hwnd, 1029, &no_mica, sizeof(no_mica));
  const MARGINS no_frame{};
  DwmExtendFrameIntoClientArea(hwnd, &no_frame);
  return false;
}

struct ThemePalette {
  winrt::Windows::UI::Color window_bg, card_bg, card_border;
  winrt::Windows::UI::Color text_primary, text_secondary;
  winrt::Windows::UI::Color btn_primary_bg, btn_primary_hover,
      btn_primary_pressed, btn_primary_fg;
  winrt::Windows::UI::Color link_fg, link_hover_fg, progress_baseline,
      progress_fill;

  static ThemePalette Create() {
    using winrt::Windows::UI::ColorHelper;
    return {
        .window_bg = ColorHelper::FromArgb(255, 244, 246, 250),
        .card_bg = ColorHelper::FromArgb(240, 255, 255, 255),
        .card_border = ColorHelper::FromArgb(68, 155, 168, 184),
        .text_primary = ColorHelper::FromArgb(255, 9, 9, 11),
        .text_secondary = ColorHelper::FromArgb(255, 74, 85, 101),
        .btn_primary_bg = ColorHelper::FromArgb(255, 37, 99, 235),
        .btn_primary_hover = ColorHelper::FromArgb(255, 47, 109, 232),
        .btn_primary_pressed = ColorHelper::FromArgb(255, 27, 80, 202),
        .btn_primary_fg = winrt::Windows::UI::Colors::White(),
        .link_fg = ColorHelper::FromArgb(255, 88, 102, 122),
        .link_hover_fg = ColorHelper::FromArgb(255, 37, 99, 235),
        .progress_baseline = ColorHelper::FromArgb(255, 209, 213, 219),
        .progress_fill = ColorHelper::FromArgb(255, 37, 99, 235),
    };
  }
};

} // namespace

struct WinUIInstallerWindow::Impl {
  UpdaterEngine *engine{nullptr};
  HWND hwnd{nullptr};
  HWND xaml_hwnd{nullptr};
  HICON app_icon_small{nullptr};
  UINT icon_dpi{0};
  ULONG_PTR gdiplus_token{0};
  std::unique_ptr<Gdiplus::Bitmap> logo_bitmap;
  std::unique_ptr<Gdiplus::Bitmap> caption_bitmap;
  std::thread worker_thread;
  int exit_code{1};
  bool close_requested{false};
  bool apartment_initialized{false};
  std::atomic<bool> status_posted{false};
  static constexpr UINT kWorkerFinished = WM_APP + 2;

  // State
  std::mutex ui_mutex;
  UpdateState current_state{UpdateState::kIdle};
  std::wstring headline_text = L"Meet Krypton";
  std::wstring stage_text = L"Fast, private, and customizable web browser";
  float target_progress{0.0f};
  float animated_progress{0.0f};
  bool is_started{false};
  bool is_uninstall{false};
  std::atomic<bool> is_completed{false};
  bool is_resumed{false};
  std::atomic<bool> has_error{false};
  bool modal_cancel_open{false};
  bool is_xaml_active{false};
  bool is_mica_active{false};
  bool has_xaml_transparency{false};
  ThemePalette palette;

  // DIP Design Constants
  static constexpr float kDesignWidth = 400.0f;
  static constexpr float kDesignHeight = 450.0f;
  static constexpr float kCaptionHeight = 32.0f;
  static constexpr DWORD kWindowStyle =
      WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
  static constexpr float kDesignActionHeight = 44.0f;

  // Scaled Physical Pixel Metrics
  float dpi_scale{1.0f};
  int phys_width{400};
  int phys_height{450};

  Gdiplus::RectF min_btn_rect;
  Gdiplus::RectF close_btn_rect;
  bool min_hovered{false};
  bool close_hovered{false};
  Gdiplus::RectF unlock_btn_rect;
  Gdiplus::RectF adv_btn_rect;
  Gdiplus::RectF launch_btn_rect;

  bool unlock_hovered{false};
  bool unlock_pressed{false};
  bool adv_hovered{false};
  bool launch_hovered{false};
  bool launch_pressed{false};
  bool tracking_mouse{false};
  bool caption_dragging{false};
  bool caption_pointer_is_mouse{true};
  uint32_t caption_pointer_id{0};
  POINT caption_pointer_start{};
  RECT caption_window_start{};
  POINT caption_window_position{};

  // System XAML Objects
  hosting::WindowsXamlManager xaml_manager{nullptr};
  hosting::DesktopWindowXamlSource xaml_source{nullptr};
  controls::Grid root_grid{nullptr};
  media::AcrylicBrush window_acrylic{nullptr};
  controls::Border logo_card{nullptr};
  shapes::Rectangle logo_surface{nullptr};
  controls::TextBlock xaml_headline{nullptr};
  controls::TextBlock xaml_subtitle{nullptr};
  controls::Grid xaml_progress_container{nullptr};
  controls::Grid xaml_progress_fill{nullptr};
  controls::Button xaml_unlock_btn{nullptr};
  controls::HyperlinkButton xaml_adv_btn{nullptr};
  controls::Button xaml_launch_btn{nullptr};
  controls::HyperlinkButton xaml_cancel_install_btn{nullptr};
  controls::ContentDialog xaml_adv_dialog{nullptr};
  controls::ContentDialog xaml_cancel_dialog{nullptr};

  void UpdateThemeAndBackdrop() {
    if (hwnd) {
      is_mica_active = ApplyMicaBackdrop(hwnd);
    }
    palette = ThemePalette::Create();
    ApplyThemeToXaml();
    if (hwnd) {
      InvalidateRect(hwnd, nullptr, TRUE);
    }
  }

  void ApplyThemeToXaml() {
    if (!is_xaml_active || !root_grid)
      return;
    root_grid.RequestedTheme(xaml::ElementTheme::Light);
    ApplyWindowBackground();
    if (logo_surface) {
      logo_surface.Fill(media::SolidColorBrush(palette.card_bg));
      logo_surface.Stroke(media::SolidColorBrush(palette.card_border));
    }
    if (xaml_headline) {
      xaml_headline.Foreground(media::SolidColorBrush(palette.text_primary));
    }
    if (xaml_subtitle) {
      xaml_subtitle.Foreground(media::SolidColorBrush(palette.text_secondary));
    }
    if (xaml_adv_btn) {
      xaml_adv_btn.Foreground(media::SolidColorBrush(palette.link_fg));
    }
  }

  void UpdateCaptionIcon(UINT dpi) {
    if (app_icon_small && icon_dpi == dpi)
      return;
    const auto icon = reinterpret_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON),
                   IMAGE_ICON, GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                   GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR));
    if (!icon)
      return;
    if (hwnd)
      SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                   reinterpret_cast<LPARAM>(icon));
    if (app_icon_small)
      DestroyIcon(app_icon_small);
    app_icon_small = icon;
    icon_dpi = dpi;
  }

  void UpdateClientLayout(int width, int height) {
    phys_width = width;
    phys_height = height;
    const float action_w = 240.0f * dpi_scale;
    const float action_h = kDesignActionHeight * dpi_scale;
    const float action_x = (phys_width - action_w) / 2.0f;
    const float caption = kCaptionHeight * dpi_scale;
    close_btn_rect = Gdiplus::RectF(phys_width - 46.0f * dpi_scale, 0.0f,
                                    46.0f * dpi_scale, caption);
    min_btn_rect = Gdiplus::RectF(phys_width - 92.0f * dpi_scale, 0.0f,
                                  46.0f * dpi_scale, caption);
    const float action_y = (kCaptionHeight + 270.0f) * dpi_scale;
    unlock_btn_rect = Gdiplus::RectF(action_x, action_y, action_w, action_h);
    launch_btn_rect = unlock_btn_rect;
    adv_btn_rect =
        Gdiplus::RectF(action_x, action_y + action_h + 8.0f * dpi_scale,
                       action_w, 24.0f * dpi_scale);
    if (xaml_hwnd) {
      SetWindowPos(xaml_hwnd, nullptr, 0, 0, phys_width, phys_height,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
  }

  void UpdateDpiMetrics(UINT dpi) {
    dpi_scale = static_cast<float>(dpi) / 96.0f;
    UpdateClientLayout(static_cast<int>(std::round(kDesignWidth * dpi_scale)),
                       static_cast<int>(std::round(kDesignHeight * dpi_scale)));
    UpdateCaptionIcon(dpi);
  }

  [[nodiscard]] SIZE OuterSize(UINT dpi) const noexcept {
    RECT bounds{0, 0, phys_width, phys_height};
    AdjustWindowRectExForDpi(&bounds, kWindowStyle, FALSE, WS_EX_APPWINDOW,
                             dpi);
    return {bounds.right - bounds.left, bounds.bottom - bounds.top};
  }

  void TrackMouse(HWND h) {
    if (!tracking_mouse) {
      TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, h, 0};
      TrackMouseEvent(&tme);
      tracking_mouse = true;
    }
  }

  [[nodiscard]] std::unique_ptr<Gdiplus::Bitmap>
  LoadResourceBitmap(UINT resource_id) {
    const HINSTANCE module = GetModuleHandleW(nullptr);
    const HRSRC resource =
        FindResourceW(module, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!resource)
      return {};
    const HGLOBAL data = LoadResource(module, resource);
    const auto bytes = static_cast<const BYTE *>(LockResource(data));
    const DWORD size = SizeofResource(module, resource);
    if (!bytes || !size)
      return {};
    winrt::com_ptr<IStream> stream;
    stream.attach(SHCreateMemStream(bytes, size));
    if (!stream)
      return {};
    const auto decoded = std::unique_ptr<Gdiplus::Bitmap>(
        Gdiplus::Bitmap::FromStream(stream.get()));
    if (!decoded || decoded->GetLastStatus() != Gdiplus::Ok)
      return {};
    // Clone before releasing the decoder stream; GDI+ may otherwise read it
    // later.
    return std::unique_ptr<Gdiplus::Bitmap>(decoded->Clone(
        0, 0, static_cast<INT>(decoded->GetWidth()),
        static_cast<INT>(decoded->GetHeight()), PixelFormat32bppPARGB));
  }

  void LoadGdiplusLogo() {
    logo_bitmap = LoadResourceBitmap(IDR_PRODUCT_LOGO);
    caption_bitmap = LoadResourceBitmap(IDR_TITLEBAR_ICON);
  }

  void ApplyWindowBackground() {
    if (!root_grid)
      return;
    try {
      if (window_acrylic) {
        root_grid.Background(window_acrylic);
        return;
      }
      media::AcrylicBrush acrylic;
      acrylic.BackgroundSource(media::AcrylicBackgroundSource::HostBackdrop);
      acrylic.TintColor(palette.window_bg);
      acrylic.TintOpacity(0.65);
      acrylic.TintLuminosityOpacity(0.85);
      acrylic.FallbackColor(palette.window_bg);
      window_acrylic = acrylic;
      root_grid.Background(acrylic);
    } catch (const winrt::hresult_error &) {
      root_grid.Background(media::SolidColorBrush(palette.window_bg));
    }
  }

  void CreateCaption() {
    controls::Grid caption;
    controls::ColumnDefinition drag_column;
    drag_column.Width({1.0, xaml::GridUnitType::Star});
    caption.ColumnDefinitions().Append(drag_column);
    for (int i = 0; i < 2; ++i) {
      controls::ColumnDefinition button_column;
      button_column.Width({46.0, xaml::GridUnitType::Pixel});
      caption.ColumnDefinitions().Append(button_column);
    }

    controls::Grid drag_area;
    drag_area.Background(
        media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
    controls::Image icon;
    icon.Width(16.0);
    icon.Height(16.0);
    icon.HorizontalAlignment(xaml::HorizontalAlignment::Left);
    icon.VerticalAlignment(xaml::VerticalAlignment::Center);
    icon.Margin(xaml::ThicknessHelper::FromLengths(12.0, 0.0, 0.0, 0.0));
    const auto module = GetModuleHandleW(nullptr);
    const auto resource =
        FindResourceW(module, MAKEINTRESOURCEW(IDR_TITLEBAR_ICON), RT_RCDATA);
    if (resource) {
      const auto data = LoadResource(module, resource);
      const auto bytes = static_cast<const BYTE *>(LockResource(data));
      const DWORD size = SizeofResource(module, resource);
      winrt::com_ptr<IStream> stream;
      stream.attach(SHCreateMemStream(bytes, size));
      streams::IRandomAccessStream random;
      if (stream && SUCCEEDED(CreateRandomAccessStreamOverStream(
                        stream.get(), BSOS_DEFAULT,
                        winrt::guid_of<streams::IRandomAccessStream>(),
                        winrt::put_abi(random)))) {
        media::Imaging::BitmapImage bitmap;
        bitmap.SetSource(random);
        icon.Source(bitmap);
      }
    }
    drag_area.Children().Append(icon);
    controls::TextBlock title;
    title.Text(is_uninstall ? L"Krypton Uninstall" : L"Krypton Setup");
    title.FontSize(13.0);
    title.Foreground(media::SolidColorBrush(palette.text_primary));
    title.VerticalAlignment(xaml::VerticalAlignment::Center);
    title.Margin(xaml::ThicknessHelper::FromLengths(36.0, 0.0, 8.0, 0.0));
    drag_area.Children().Append(title);
    // Capture the caption's pointer directly; moving the window must not depend
    // on XAML forwarding a queued native system command.
    icon.IsHitTestVisible(false);
    title.IsHitTestVisible(false);
    drag_area.ManipulationMode(xaml::Input::ManipulationModes::None);
    drag_area.PointerPressed([this](auto const &sender, auto const &args) {
      if (caption_dragging)
        return;
      const auto pointer = args.Pointer();
      caption_pointer_is_mouse =
          pointer.PointerDeviceType() ==
          winrt::Windows::Devices::Input::PointerDeviceType::Mouse;
      if (caption_pointer_is_mouse &&
          !args.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed())
        return;
      caption_pointer_id = pointer.PointerId();
      if (!CaptionPointerPosition(caption_pointer_start) ||
          !GetWindowRect(hwnd, &caption_window_start))
        return;
      const auto area = sender.template as<controls::Grid>();
      if (!area.CapturePointer(pointer))
        return;
      caption_window_position = {caption_window_start.left,
                                 caption_window_start.top};
      caption_dragging = true;
      args.Handled(true);
    });
    drag_area.PointerMoved([this](auto const &, auto const &args) {
      if (!caption_dragging || args.Pointer().PointerId() != caption_pointer_id)
        return;
      POINT position{};
      if (!CaptionPointerPosition(position))
        return;
      const int x =
          caption_window_start.left + position.x - caption_pointer_start.x;
      const int y =
          caption_window_start.top + position.y - caption_pointer_start.y;
      if (x != caption_window_position.x || y != caption_window_position.y) {
        caption_window_position = {x, y};
        if (!SetWindowPos(hwnd, nullptr, x, y, 0, 0,
                          SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
          caption_dragging = false;
        }
      }
      args.Handled(true);
    });
    drag_area.PointerReleased([this](auto const &sender, auto const &args) {
      if (!caption_dragging || args.Pointer().PointerId() != caption_pointer_id)
        return;
      caption_dragging = false;
      sender.template as<controls::Grid>().ReleasePointerCapture(
          args.Pointer());
      args.Handled(true);
    });
    drag_area.PointerCanceled([this](auto const &, auto const &args) {
      if (args.Pointer().PointerId() == caption_pointer_id)
        caption_dragging = false;
    });
    drag_area.PointerCaptureLost([this](auto const &, auto const &args) {
      if (args.Pointer().PointerId() == caption_pointer_id)
        caption_dragging = false;
    });
    caption.Children().Append(drag_area);

    auto make_button = [](const wchar_t *glyph, bool close) {
      controls::Button button;
      button.Content(winrt::box_value(winrt::hstring(glyph)));
      button.FontFamily(media::FontFamily(L"Segoe MDL2 Assets"));
      button.FontSize(10.0);
      button.IsTabStop(false);
      winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(
          button, close ? L"Close" : L"Minimize");
      button.Height(kCaptionHeight);
      button.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
      button.VerticalAlignment(xaml::VerticalAlignment::Stretch);
      button.Padding(xaml::ThicknessHelper::FromUniformLength(0.0));
      button.BorderThickness(xaml::ThicknessHelper::FromUniformLength(0.0));
      button.CornerRadius(xaml::CornerRadiusHelper::FromUniformRadius(0.0));
      button.Background(
          media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
      button.Foreground(
          media::SolidColorBrush(winrt::Windows::UI::Colors::Black()));
      const auto hover =
          close ? winrt::Windows::UI::ColorHelper::FromArgb(255, 196, 43, 28)
                : winrt::Windows::UI::ColorHelper::FromArgb(24, 0, 0, 0);
      const auto foreground = close ? winrt::Windows::UI::Colors::White()
                                    : winrt::Windows::UI::Colors::Black();
      for (const auto key :
           {L"ButtonBackgroundPointerOver", L"ButtonBackgroundPressed"}) {
        button.Resources().Insert(winrt::box_value(winrt::hstring(key)),
                                  media::SolidColorBrush(hover));
      }
      for (const auto key :
           {L"ButtonForegroundPointerOver", L"ButtonForegroundPressed"}) {
        button.Resources().Insert(winrt::box_value(winrt::hstring(key)),
                                  media::SolidColorBrush(foreground));
      }
      return button;
    };
    auto minimize = make_button(L"\uE921", false);
    minimize.Click(
        [this](auto const &, auto const &) { ShowWindow(hwnd, SW_MINIMIZE); });
    controls::Grid::SetColumn(minimize, 1);
    caption.Children().Append(minimize);
    auto close = make_button(L"\uE8BB", true);
    close.Click(
        [this](auto const &, auto const &) { PromptCancelConfirmation(); });
    controls::Grid::SetColumn(close, 2);
    caption.Children().Append(close);
    root_grid.Children().Append(caption);
  }

  void RequestClose(bool cancel) {
    if (!worker_thread.joinable()) {
      PostQuitMessage(exit_code);
      return;
    }
    close_requested = true;
    if (cancel && engine)
      engine->Cancel();
  }

  void NotifyStatus() {
    if (hwnd && !status_posted.exchange(true) &&
        !PostMessageW(hwnd, WM_USER + 1, 0, 0))
      status_posted = false;
  }

  void ReleaseXaml() noexcept {
    is_xaml_active = false;
    if (xaml_source) {
      try {
        xaml_source.Content(nullptr);
      } catch (const winrt::hresult_error &) {
        // The island can already be closed by the platform.
      }
    }
    xaml_cancel_dialog = nullptr;
    xaml_adv_dialog = nullptr;
    xaml_cancel_install_btn = nullptr;
    xaml_launch_btn = nullptr;
    xaml_adv_btn = nullptr;
    xaml_unlock_btn = nullptr;
    xaml_progress_fill = nullptr;
    xaml_progress_container = nullptr;
    xaml_subtitle = nullptr;
    xaml_headline = nullptr;
    logo_surface = nullptr;
    logo_card = nullptr;
    window_acrylic = nullptr;
    root_grid = nullptr;
    if (xaml_source) {
      try {
        xaml_source.Close();
      } catch (const winrt::hresult_error &) {
      }
      xaml_source = nullptr;
    }
    if (xaml_manager) {
      try {
        xaml_manager.Close();
      } catch (const winrt::hresult_error &) {
      }
      xaml_manager = nullptr;
    }
    xaml_hwnd = nullptr;
  }

  void StartInstallation() {
    if (is_started)
      return;
    is_started = true;
    headline_text =
        is_uninstall ? L"Removing Krypton..." : L"Preparing installation...";
    stage_text = is_uninstall ? L"Your browsing data will be kept."
                 : is_resumed ? L"Right back where we left off..."
                              : L"A fresh start begins here...";

    if (is_xaml_active) {
      if (xaml_unlock_btn)
        xaml_unlock_btn.Visibility(xaml::Visibility::Collapsed);
      if (xaml_adv_btn)
        xaml_adv_btn.Visibility(xaml::Visibility::Collapsed);
      if (xaml_progress_container)
        xaml_progress_container.Visibility(xaml::Visibility::Visible);
      if (xaml_cancel_install_btn)
        xaml_cancel_install_btn.Visibility(xaml::Visibility::Visible);
      if (xaml_headline)
        xaml_headline.Text(headline_text);
      if (xaml_subtitle)
        xaml_subtitle.Text(stage_text);
    } else {
      InvalidateRect(hwnd, nullptr, FALSE);
    }

    SetTimer(hwnd, 1, 16, nullptr);
    worker_thread = std::thread([this, window = hwnd] {
      exit_code = engine ? engine->Execute() : 1;
      PostMessageW(window, kWorkerFinished, 0, 0);
    });
  }

  void CreateSystemXamlUI() {
    root_grid = controls::Grid();
    root_grid.RequestedTheme(xaml::ElementTheme::Light);
    ApplyWindowBackground();
    controls::RowDefinition caption_row;
    caption_row.Height({kCaptionHeight, xaml::GridUnitType::Pixel});
    controls::RowDefinition body_row;
    body_row.Height({1.0, xaml::GridUnitType::Star});
    root_grid.RowDefinitions().Append(caption_row);
    root_grid.RowDefinitions().Append(body_row);
    CreateCaption();

    controls::StackPanel panel;
    panel.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
    panel.VerticalAlignment(xaml::VerticalAlignment::Stretch);
    panel.Spacing(8.0);
    controls::Grid::SetRow(panel, 1);

    // 1. 128x128 Floating Brand Card
    logo_card = controls::Border();
    logo_card.Width(128.0);
    logo_card.Height(128.0);
    logo_card.Margin(xaml::ThicknessHelper::FromLengths(0.0, 32.0, 0.0, 14.0));
    logo_card.HorizontalAlignment(xaml::HorizontalAlignment::Center);
    logo_card.CornerRadius(xaml::CornerRadiusHelper::FromUniformRadius(20.0));
    logo_card.BorderThickness(xaml::ThicknessHelper::FromUniformLength(0.0));
    controls::Grid logo_layers;
    controls::Grid shadow_host;
    shadow_host.IsHitTestVisible(false);
    logo_layers.Children().Append(shadow_host);

    logo_surface = shapes::Rectangle();
    logo_surface.Width(128.0);
    logo_surface.Height(128.0);
    logo_surface.RadiusX(20.0);
    logo_surface.RadiusY(20.0);
    logo_surface.Fill(media::SolidColorBrush(palette.card_bg));
    logo_surface.Stroke(media::SolidColorBrush(palette.card_border));
    logo_surface.StrokeThickness(1.0);
    logo_layers.Children().Append(logo_surface);

    // The alpha mask follows the rounded card instead of its bounding
    // rectangle.
    const auto compositor =
        hosting::ElementCompositionPreview::GetElementVisual(shadow_host)
            .Compositor();
    const auto shadow = compositor.CreateDropShadow();
    shadow.Color(winrt::Windows::UI::Colors::Black());
    shadow.BlurRadius(12.0f);
    shadow.Opacity(0.12f);
    shadow.Offset({0.0f, 3.0f, 0.0f});
    shadow.Mask(logo_surface.GetAlphaMask());
    const auto shadow_visual = compositor.CreateSpriteVisual();
    shadow_visual.Size({128.0f, 128.0f});
    shadow_visual.Shadow(shadow);
    hosting::ElementCompositionPreview::SetElementChildVisual(shadow_host,
                                                              shadow_visual);

    controls::Image logo_img;
    logo_img.Width(100.0);
    logo_img.Height(100.0);
    logo_img.Stretch(media::Stretch::Uniform);
    logo_img.HorizontalAlignment(xaml::HorizontalAlignment::Center);
    logo_img.VerticalAlignment(xaml::VerticalAlignment::Center);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HRSRC hRes =
        FindResourceW(hInst, MAKEINTRESOURCEW(IDR_PRODUCT_LOGO), RT_RCDATA);
    if (hRes) {
      HGLOBAL hData = LoadResource(hInst, hRes);
      DWORD size = SizeofResource(hInst, hRes);
      void *ptr = LockResource(hData);
      if (ptr && size > 0) {
        IStream *pStream =
            SHCreateMemStream(reinterpret_cast<const BYTE *>(ptr), size);
        if (pStream) {
          streams::IRandomAccessStream ras{nullptr};
          if (SUCCEEDED(CreateRandomAccessStreamOverStream(
                  pStream, BSOS_DEFAULT,
                  winrt::guid_of<streams::IRandomAccessStream>(),
                  winrt::put_abi(ras)))) {
            media::Imaging::BitmapImage bmp;
            bmp.SetSource(ras);
            logo_img.Source(bmp);
          }
          pStream->Release();
        }
      }
    }
    logo_layers.Children().Append(logo_img);
    logo_card.Child(logo_layers);
    panel.Children().Append(logo_card);

    // 2. Headline TextBlock
    xaml_headline = controls::TextBlock();
    xaml_headline.Text(headline_text);
    xaml_headline.FontSize(26.0);
    xaml_headline.FontWeight(text::FontWeights::SemiBold());
    xaml_headline.Foreground(media::SolidColorBrush(palette.text_primary));
    xaml_headline.HorizontalAlignment(xaml::HorizontalAlignment::Center);
    xaml_headline.Margin(
        xaml::ThicknessHelper::FromLengths(16.0, 0.0, 16.0, 4.0));
    panel.Children().Append(xaml_headline);

    // 3. Status Subtitle
    xaml_subtitle = controls::TextBlock();
    xaml_subtitle.Text(stage_text);
    xaml_subtitle.FontSize(14.0);
    xaml_subtitle.Foreground(media::SolidColorBrush(palette.text_secondary));
    xaml_subtitle.HorizontalAlignment(xaml::HorizontalAlignment::Center);
    xaml_subtitle.TextAlignment(xaml::TextAlignment::Center);
    xaml_subtitle.TextWrapping(xaml::TextWrapping::Wrap);
    xaml_subtitle.Margin(
        xaml::ThicknessHelper::FromLengths(24.0, 0.0, 24.0, 18.0));
    panel.Children().Append(xaml_subtitle);

    // 4. Primary "Unlock the Experience" Button (clean typography, no download
    // icon, hover-fixed)
    xaml_unlock_btn = controls::Button();
    xaml_unlock_btn.Content(winrt::box_value(winrt::hstring(
        (is_uninstall ? L"Uninstall Krypton" : L"Unlock the Experience"))));
    xaml_unlock_btn.Width(240.0);
    xaml_unlock_btn.Height(kDesignActionHeight);
    xaml_unlock_btn.HorizontalAlignment(xaml::HorizontalAlignment::Center);
    xaml_unlock_btn.Background(media::SolidColorBrush(palette.btn_primary_bg));
    xaml_unlock_btn.Foreground(media::SolidColorBrush(palette.btn_primary_fg));
    xaml_unlock_btn.FontWeight(text::FontWeights::SemiBold());
    xaml_unlock_btn.FontSize(14.0);
    xaml_unlock_btn.CornerRadius(
        xaml::CornerRadiusHelper::FromUniformRadius(10.0));
    xaml_unlock_btn.BorderThickness(
        xaml::ThicknessHelper::FromUniformLength(0.0));

    // Override button theme brushes to prevent white-on-white hover bug
    xaml_unlock_btn.Resources().Insert(
        winrt::box_value(L"ButtonBackgroundPointerOver"),
        media::SolidColorBrush(palette.btn_primary_hover));
    xaml_unlock_btn.Resources().Insert(
        winrt::box_value(L"ButtonForegroundPointerOver"),
        media::SolidColorBrush(palette.btn_primary_fg));
    xaml_unlock_btn.Resources().Insert(
        winrt::box_value(L"ButtonBackgroundPressed"),
        media::SolidColorBrush(palette.btn_primary_pressed));
    xaml_unlock_btn.Resources().Insert(
        winrt::box_value(L"ButtonForegroundPressed"),
        media::SolidColorBrush(palette.btn_primary_fg));

    xaml_unlock_btn.Click(
        [this](auto const &, auto const &) { StartInstallation(); });
    panel.Children().Append(xaml_unlock_btn);

    // 5. Link-style "Advanced options" HyperlinkButton (no solid hover box)
    xaml_adv_btn = controls::HyperlinkButton();
    xaml_adv_btn.Content(winrt::box_value(winrt::hstring(L"Advanced options")));
    xaml_adv_btn.FontSize(13.0);
    xaml_adv_btn.Foreground(media::SolidColorBrush(palette.link_fg));
    xaml_adv_btn.HorizontalAlignment(xaml::HorizontalAlignment::Center);
    xaml_adv_btn.Margin(xaml::ThicknessHelper::FromLengths(0.0, 8.0, 0.0, 0.0));
    xaml_adv_btn.Padding(
        xaml::ThicknessHelper::FromLengths(8.0, 3.0, 8.0, 3.0));

    xaml_adv_btn.Resources().Insert(
        winrt::box_value(L"HyperlinkButtonForegroundPointerOver"),
        media::SolidColorBrush(palette.link_hover_fg));

    xaml_adv_btn.Click([this](auto const &, auto const &) {
      if (xaml_adv_dialog) {
        xaml_adv_dialog.ShowAsync();
      }
    });
    panel.Children().Append(xaml_adv_btn);
    if (is_uninstall)
      xaml_adv_btn.Visibility(xaml::Visibility::Collapsed);

    // 6. Layered Progress Bar (Hidden until installation starts)
    xaml_progress_container = controls::Grid();
    xaml_progress_container.Width(256.0);
    xaml_progress_container.Height(6.0);
    xaml_progress_container.HorizontalAlignment(
        xaml::HorizontalAlignment::Center);
    xaml_progress_container.Margin(
        xaml::ThicknessHelper::FromLengths(0.0, 10.0, 0.0, 0.0));
    xaml_progress_container.Visibility(xaml::Visibility::Collapsed);

    controls::Grid baseline_track;
    baseline_track.Width(256.0);
    baseline_track.Height(2.0);
    baseline_track.CornerRadius(
        xaml::CornerRadiusHelper::FromUniformRadius(2.0));
    baseline_track.Background(
        media::SolidColorBrush(palette.progress_baseline));
    baseline_track.VerticalAlignment(xaml::VerticalAlignment::Center);
    xaml_progress_container.Children().Append(baseline_track);

    xaml_progress_fill = controls::Grid();
    xaml_progress_fill.Width(0.0);
    xaml_progress_fill.Height(4.0);
    xaml_progress_fill.CornerRadius(
        xaml::CornerRadiusHelper::FromUniformRadius(2.0));
    xaml_progress_fill.Background(
        media::SolidColorBrush(palette.progress_fill));
    xaml_progress_fill.HorizontalAlignment(xaml::HorizontalAlignment::Left);
    xaml_progress_fill.VerticalAlignment(xaml::VerticalAlignment::Center);
    xaml_progress_container.Children().Append(xaml_progress_fill);
    panel.Children().Append(xaml_progress_container);
    xaml_cancel_install_btn = controls::HyperlinkButton();
    xaml_cancel_install_btn.Content(
        winrt::box_value(winrt::hstring(L"Cancel installation")));
    xaml_cancel_install_btn.Foreground(media::SolidColorBrush(palette.link_fg));
    xaml_cancel_install_btn.FontSize(13.0);
    xaml_cancel_install_btn.Resources().Insert(
        winrt::box_value(L"HyperlinkButtonForegroundPointerOver"),
        media::SolidColorBrush(palette.link_hover_fg));
    xaml_cancel_install_btn.HorizontalAlignment(
        xaml::HorizontalAlignment::Center);
    xaml_cancel_install_btn.Visibility(xaml::Visibility::Collapsed);
    xaml_cancel_install_btn.Click(
        [this](auto const &, auto const &) { PromptCancelConfirmation(); });
    panel.Children().Append(xaml_cancel_install_btn);

    // 7. "Launch Krypton" Button (Visible only on completion)
    xaml_launch_btn = controls::Button();
    xaml_launch_btn.Content(winrt::box_value(
        winrt::hstring((is_uninstall ? L"Done" : L"Launch Krypton \u2192"))));
    xaml_launch_btn.Width(240.0);
    xaml_launch_btn.Height(kDesignActionHeight);
    xaml_launch_btn.HorizontalAlignment(xaml::HorizontalAlignment::Center);
    xaml_launch_btn.Background(media::SolidColorBrush(
        winrt::Windows::UI::ColorHelper::FromArgb(255, 16, 185, 129)));
    xaml_launch_btn.Foreground(
        media::SolidColorBrush(winrt::Windows::UI::Colors::White()));
    xaml_launch_btn.FontWeight(text::FontWeights::SemiBold());
    xaml_launch_btn.FontSize(14.0);
    xaml_launch_btn.CornerRadius(
        xaml::CornerRadiusHelper::FromUniformRadius(10.0));
    xaml_launch_btn.BorderThickness(
        xaml::ThicknessHelper::FromUniformLength(0.0));
    xaml_launch_btn.Resources().Insert(
        winrt::box_value(L"ButtonBackgroundPointerOver"),
        media::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(255, 5, 150, 105)));
    xaml_launch_btn.Resources().Insert(
        winrt::box_value(L"ButtonForegroundPointerOver"),
        media::SolidColorBrush(winrt::Windows::UI::Colors::White()));
    xaml_launch_btn.Resources().Insert(
        winrt::box_value(L"ButtonBackgroundPressed"),
        media::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(255, 4, 120, 87)));
    xaml_launch_btn.Resources().Insert(
        winrt::box_value(L"ButtonForegroundPressed"),
        media::SolidColorBrush(winrt::Windows::UI::Colors::White()));
    xaml_launch_btn.Visibility(xaml::Visibility::Collapsed);
    xaml_launch_btn.Click([this](auto const &, auto const &) {
      if (engine) {
        auto launcher = engine->InstalledExecutable();
        if (!launcher.empty())
          ShellExecuteW(nullptr, L"open", launcher.c_str(), nullptr, nullptr,
                        SW_SHOW);
      }
      RequestClose(false);
    });
    panel.Children().Append(xaml_launch_btn);

    root_grid.Children().Append(panel);

    // 8. Advanced Options Dialog
    xaml_adv_dialog = controls::ContentDialog();
    xaml_adv_dialog.Title(
        winrt::box_value(winrt::hstring(L"Advanced Options")));
    xaml_adv_dialog.Foreground(media::SolidColorBrush(palette.text_primary));
    xaml_adv_dialog.Background(
        media::SolidColorBrush(winrt::Windows::UI::Colors::White()));
    xaml_adv_dialog.CornerRadius(
        xaml::CornerRadiusHelper::FromUniformRadius(10.0));
    xaml_adv_dialog.RequestedTheme(xaml::ElementTheme::Light);

    controls::StackPanel adv_panel;
    adv_panel.Spacing(12.0);
    adv_panel.Width(300.0);

    controls::TextBlock loc_label;
    loc_label.Text(L"Install Location:");
    loc_label.FontSize(13.0);
    loc_label.FontWeight(text::FontWeights::SemiBold());
    loc_label.Foreground(media::SolidColorBrush(palette.text_primary));
    adv_panel.Children().Append(loc_label);

    controls::Grid loc_grid;
    controls::ColumnDefinition col1;
    col1.Width(xaml::GridLength{1.0, xaml::GridUnitType::Star});
    controls::ColumnDefinition col2;
    col2.Width(xaml::GridLength{0.0, xaml::GridUnitType::Auto});
    loc_grid.ColumnDefinitions().Append(col1);
    loc_grid.ColumnDefinitions().Append(col2);

    controls::TextBox path_box;
    std::wstring current_dir;
    if (engine && !engine->Options().install_root.empty()) {
      current_dir = engine->Options().install_root.wstring();
    } else {
      PWSTR local_app = nullptr;
      if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                         &local_app))) {
        current_dir = (std::filesystem::path(local_app) / "Krypton").wstring();
        CoTaskMemFree(local_app);
      }
    }
    path_box.Text(current_dir);
    path_box.IsReadOnly(true);
    path_box.FontSize(12.0);
    path_box.Foreground(media::SolidColorBrush(palette.text_primary));
    path_box.Background(
        media::SolidColorBrush(winrt::Windows::UI::Colors::White()));
    path_box.Margin(xaml::ThicknessHelper::FromLengths(0.0, 0.0, 8.0, 0.0));
    controls::Grid::SetColumn(path_box, 0);
    loc_grid.Children().Append(path_box);

    controls::Button browse_btn;
    browse_btn.IsEnabled(false);
    browse_btn.Content(winrt::box_value(winrt::hstring(L"Browse...")));
    StyleDialogButton(browse_btn, false);
    browse_btn.Width(76.0);
    browse_btn.FontSize(12.0);
    controls::Grid::SetColumn(browse_btn, 1);
    browse_btn.Click([this, path_box](auto const &, auto const &) {
      std::wstring picked = PickFolderDialog(hwnd, path_box.Text().c_str());
      if (!picked.empty()) {
        path_box.Text(picked);
      }
    });
    loc_grid.Children().Append(browse_btn);
    adv_panel.Children().Append(loc_grid);

    controls::CheckBox desktop_chk;
    desktop_chk.Foreground(media::SolidColorBrush(palette.text_primary));
    controls::TextBlock desktop_label;
    desktop_label.Text(L"Create Desktop shortcut");
    desktop_label.FontSize(13.0);
    desktop_label.Foreground(media::SolidColorBrush(palette.text_primary));
    desktop_chk.Content(desktop_label);
    desktop_chk.IsChecked(engine ? engine->Options().create_desktop_shortcut
                                 : true);
    adv_panel.Children().Append(desktop_chk);

    controls::CheckBox start_chk;
    start_chk.Foreground(media::SolidColorBrush(palette.text_primary));
    controls::TextBlock start_label;
    start_label.Text(L"Create Start Menu shortcut");
    start_label.FontSize(13.0);
    start_label.Foreground(media::SolidColorBrush(palette.text_primary));
    start_chk.Content(start_label);
    start_chk.IsChecked(engine ? engine->Options().create_start_menu_shortcut
                               : true);
    adv_panel.Children().Append(start_chk);

    controls::StackPanel advanced_actions;
    advanced_actions.Orientation(controls::Orientation::Horizontal);
    advanced_actions.Spacing(12.0);
    advanced_actions.Margin(
        xaml::ThicknessHelper::FromLengths(0.0, 8.0, 0.0, 0.0));
    controls::Button discard;
    discard.Content(winrt::box_value(winrt::hstring(L"Cancel")));
    StyleDialogButton(discard, false);
    discard.Click(
        [this](auto const &, auto const &) { xaml_adv_dialog.Hide(); });
    advanced_actions.Children().Append(discard);
    controls::Button save;
    save.Content(winrt::box_value(winrt::hstring(L"Save")));
    StyleDialogButton(save, true);
    save.Click(
        [this, path_box, desktop_chk, start_chk](auto const &, auto const &) {
          if (engine) {
            engine->MutableOptions().install_root = path_box.Text().c_str();
            engine->MutableOptions().create_desktop_shortcut =
                desktop_chk.IsChecked().GetBoolean();
            engine->MutableOptions().create_start_menu_shortcut =
                start_chk.IsChecked().GetBoolean();
          }
          xaml_adv_dialog.Hide();
        });
    advanced_actions.Children().Append(save);
    adv_panel.Children().Append(advanced_actions);
    xaml_adv_dialog.Content(adv_panel);

    controls::Grid::SetRowSpan(xaml_adv_dialog, 2);
    root_grid.Children().Append(xaml_adv_dialog);

    // Explicit action buttons keep text readable in bare System XAML islands.
    xaml_cancel_dialog = controls::ContentDialog();
    xaml_cancel_dialog.Title(
        winrt::box_value(winrt::hstring(L"Cancel installation?")));
    xaml_cancel_dialog.RequestedTheme(xaml::ElementTheme::Light);
    xaml_cancel_dialog.Foreground(media::SolidColorBrush(palette.text_primary));
    xaml_cancel_dialog.Background(
        media::SolidColorBrush(winrt::Windows::UI::Colors::White()));
    xaml_cancel_dialog.CornerRadius(
        xaml::CornerRadiusHelper::FromUniformRadius(10.0));
    controls::StackPanel cancel_panel;
    cancel_panel.Width(300.0);
    cancel_panel.Spacing(20.0);
    controls::TextBlock explanation;
    explanation.Text(
        L"Setup will stop. You can run the installer again later.");
    explanation.TextWrapping(xaml::TextWrapping::Wrap);
    explanation.Foreground(media::SolidColorBrush(palette.text_secondary));
    cancel_panel.Children().Append(explanation);
    controls::StackPanel cancel_actions;
    cancel_actions.Orientation(controls::Orientation::Horizontal);
    cancel_actions.Spacing(12.0);
    controls::Button keep_installing;
    keep_installing.Content(
        winrt::box_value(winrt::hstring(L"Keep installing")));
    StyleDialogButton(keep_installing, true);
    keep_installing.Click(
        [this](auto const &, auto const &) { xaml_cancel_dialog.Hide(); });
    cancel_actions.Children().Append(keep_installing);
    controls::Button cancel_installation;
    cancel_installation.Content(
        winrt::box_value(winrt::hstring(L"Cancel installation")));
    StyleDialogButton(cancel_installation, false);
    cancel_installation.Click([this](auto const &, auto const &) {
      xaml_cancel_dialog.Hide();
      RequestClose(true);
    });
    cancel_actions.Children().Append(cancel_installation);
    cancel_panel.Children().Append(cancel_actions);
    xaml_cancel_dialog.Content(cancel_panel);
    xaml_cancel_dialog.Closed(
        [this](auto const &, auto const &) { modal_cancel_open = false; });
    controls::Grid::SetRowSpan(xaml_cancel_dialog, 2);
    root_grid.Children().Append(xaml_cancel_dialog);

    xaml_source.Content(root_grid);
    is_xaml_active = true;
  }

  void StyleDialogButton(controls::Button const &button, bool primary) {
    button.Width(144.0);
    button.Height(36.0);
    button.FontSize(13.0);
    button.CornerRadius(xaml::CornerRadiusHelper::FromUniformRadius(6.0));
    button.BorderThickness(
        xaml::ThicknessHelper::FromUniformLength(primary ? 0.0 : 1.0));
    button.BorderBrush(media::SolidColorBrush(palette.card_border));
    const auto foreground =
        primary ? palette.btn_primary_fg : palette.text_primary;
    button.Foreground(media::SolidColorBrush(foreground));
    button.Background(media::SolidColorBrush(primary ? palette.btn_primary_bg
                                                     : palette.window_bg));
    const auto hover =
        primary ? palette.btn_primary_hover
                : winrt::Windows::UI::ColorHelper::FromArgb(255, 230, 234, 240);
    for (const auto key :
         {L"ButtonBackgroundPointerOver", L"ButtonBackgroundPressed"}) {
      button.Resources().Insert(winrt::box_value(winrt::hstring(key)),
                                media::SolidColorBrush(hover));
    }
    for (const auto key :
         {L"ButtonForegroundPointerOver", L"ButtonForegroundPressed"}) {
      button.Resources().Insert(winrt::box_value(winrt::hstring(key)),
                                media::SolidColorBrush(foreground));
    }
  }

  void PromptCancelConfirmation() {
    if (modal_cancel_open || close_requested)
      return;
    if (is_completed || has_error || !is_started) {
      RequestClose(false);
      return;
    }
    if (is_xaml_active && xaml_cancel_dialog) {
      try {
        modal_cancel_open = true;
        xaml_cancel_dialog.ShowAsync();
        return;
      } catch (const winrt::hresult_error &) {
        modal_cancel_open = false;
      }
    }
    modal_cancel_open = true;
    const int answer = MessageBoxW(hwnd, L"Do you want to cancel installation?",
                                   L"Krypton Setup",
                                   MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    modal_cancel_open = false;
    if (answer == IDYES) {
      RequestClose(true);
    }
  }

  [[nodiscard]] bool CaptionPointerPosition(POINT &position) const noexcept {
    if (!caption_pointer_is_mouse) {
      POINTER_INFO information{};
      if (GetPointerInfo(caption_pointer_id, &information)) {
        position = information.ptPixelLocation;
        return true;
      }
    }
    return GetCursorPos(&position) != FALSE;
  }

  [[nodiscard]] bool IsCaptionDragPoint(LPARAM screen_position) const noexcept {
    POINT point{GET_X_LPARAM(screen_position), GET_Y_LPARAM(screen_position)};
    ScreenToClient(hwnd, &point);
    const Gdiplus::PointF hit(static_cast<float>(point.x),
                              static_cast<float>(point.y));
    return point.y >= 0 && point.y < kCaptionHeight * dpi_scale &&
           !min_btn_rect.Contains(hit) && !close_btn_rect.Contains(hit);
  }

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *self =
        reinterpret_cast<Impl *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
      auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
      self = reinterpret_cast<Impl *>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      self->hwnd = hwnd;
      return 0;
    }

    case WM_NCHITTEST:
      if (self && self->IsCaptionDragPoint(lp))
        return HTCAPTION;
      break;
    case WM_SYSCOMMAND:
      // The DWM frame provides materials and shadows; setup remains fixed-size.
      if ((wp & 0xFFF0) == SC_SIZE || (wp & 0xFFF0) == SC_MAXIMIZE) {
        return 0;
      }
      break;
    case WM_CLOSE:
      if (self) {
        self->PromptCancelConfirmation();
        return 0;
      }
      break;
    case WM_ACTIVATE:
      if (self) {
        self->UpdateThemeAndBackdrop();
      }
      break;
    case WM_SETTINGCHANGE:
    case WM_DWMCOMPOSITIONCHANGED: {
      if (self) {
        self->UpdateThemeAndBackdrop();
      }
      return 0;
    }

    case WM_SIZE:
      if (self && wp != SIZE_MINIMIZED) {
        self->UpdateClientLayout(LOWORD(lp), HIWORD(lp));
      }
      return 0;

    case WM_DPICHANGED: {
      if (self) {
        UINT new_dpi = LOWORD(wp);
        self->UpdateDpiMetrics(new_dpi);
        auto *prc = reinterpret_cast<RECT *>(lp);
        SetWindowPos(hwnd, nullptr, prc->left, prc->top, prc->right - prc->left,
                     prc->bottom - prc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (self->caption_dragging) {
          self->caption_dragging =
              self->CaptionPointerPosition(self->caption_pointer_start) &&
              GetWindowRect(hwnd, &self->caption_window_start);
          if (self->caption_dragging) {
            self->caption_window_position = {self->caption_window_start.left,
                                             self->caption_window_start.top};
          }
        }
      }
      return 0;
    }

    case WM_TIMER: {
      if (wp == 1 && self && self->is_started) {
        std::lock_guard lock(self->ui_mutex);
        float diff = self->target_progress - self->animated_progress;
        if (std::abs(diff) > 0.001f) {
          self->animated_progress += diff * 0.15f;
          if (self->is_xaml_active && self->xaml_progress_fill) {
            double bar_w =
                256.0 * std::clamp(self->animated_progress, 0.0f, 1.0f);
            self->xaml_progress_fill.Width(bar_w);
          } else {
            InvalidateRect(hwnd, nullptr, FALSE);
          }
        }
      }
      return 0;
    }

    case WM_MOUSEMOVE:
      if (self && !self->is_xaml_active) {
        self->TrackMouse(hwnd);
        const Gdiplus::PointF pt(static_cast<float>(GET_X_LPARAM(lp)),
                                 static_cast<float>(GET_Y_LPARAM(lp)));
        const bool min = self->min_btn_rect.Contains(pt);
        const bool close = self->close_btn_rect.Contains(pt);
        const bool unlock =
            !self->is_started && self->unlock_btn_rect.Contains(pt);
        const bool advanced =
            !self->is_completed &&
            (!self->is_uninstall && self->adv_btn_rect.Contains(pt));
        const bool launch =
            self->is_completed && self->launch_btn_rect.Contains(pt);
        if (min != self->min_hovered || close != self->close_hovered ||
            unlock != self->unlock_hovered || advanced != self->adv_hovered ||
            launch != self->launch_hovered) {
          self->min_hovered = min;
          self->close_hovered = close;
          self->unlock_hovered = unlock;
          self->adv_hovered = advanced;
          self->launch_hovered = launch;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
      }
      return 0;

    case WM_MOUSELEAVE:
      if (self) {
        self->tracking_mouse = false;
        self->min_hovered = false;
        self->close_hovered = false;
        self->unlock_hovered = false;
        self->adv_hovered = false;
        self->launch_hovered = false;
        if (!self->is_xaml_active)
          InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;

    case WM_LBUTTONDOWN:
      if (self && !self->is_xaml_active) {
        const Gdiplus::PointF pt(static_cast<float>(GET_X_LPARAM(lp)),
                                 static_cast<float>(GET_Y_LPARAM(lp)));
        self->unlock_pressed =
            !self->is_started && self->unlock_btn_rect.Contains(pt);
        self->launch_pressed =
            self->is_completed && self->launch_btn_rect.Contains(pt);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;

    case WM_LBUTTONUP:
      if (self && !self->is_xaml_active) {
        const Gdiplus::PointF pt(static_cast<float>(GET_X_LPARAM(lp)),
                                 static_cast<float>(GET_Y_LPARAM(lp)));
        if (self->min_btn_rect.Contains(pt)) {
          ShowWindow(hwnd, SW_MINIMIZE);
        } else if (self->close_btn_rect.Contains(pt)) {
          self->PromptCancelConfirmation();
        } else if (!self->is_started && self->unlock_pressed &&
                   self->unlock_btn_rect.Contains(pt)) {
          self->StartInstallation();
        } else if (self->is_started && !self->is_completed &&
                   self->adv_btn_rect.Contains(pt)) {
          self->PromptCancelConfirmation();
        } else if (!self->is_started &&
                   (!self->is_uninstall && self->adv_btn_rect.Contains(pt))) {
          const std::wstring current =
              self->engine ? self->engine->Options().install_root.wstring()
                           : L"";
          const auto picked = PickFolderDialog(hwnd, current);
          if (!picked.empty() && self->engine) {
            self->engine->MutableOptions().install_root = picked;
          }
        } else if (self->is_completed && self->launch_pressed &&
                   self->launch_btn_rect.Contains(pt)) {
          if (self->engine) {
            const auto launcher = self->engine->InstalledExecutable();
            if (!launcher.empty())
              ShellExecuteW(nullptr, L"open", launcher.c_str(), nullptr,
                            nullptr, SW_SHOW);
          }
          self->RequestClose(false);
        }
        self->unlock_pressed = false;
        self->launch_pressed = false;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      if (self) {
        self->RenderCaptionAndFallback(hdc);
      }
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;

    case WM_USER + 1: { // UI state change dispatch
      if (self) {
        self->status_posted = false;
        std::wstring headline;
        std::wstring stage;
        {
          std::lock_guard lock(self->ui_mutex);
          headline = self->headline_text;
          stage = self->stage_text;
        }
        if (self->is_xaml_active) {
          if (self->xaml_headline)
            self->xaml_headline.Text(headline);
          if (self->xaml_subtitle)
            self->xaml_subtitle.Text(stage);
          if (self->has_error && self->xaml_cancel_install_btn) {
            self->xaml_cancel_install_btn.Content(
                winrt::box_value(winrt::hstring(L"Close")));
          }
          if (self->is_completed) {
            if (self->xaml_progress_container)
              self->xaml_progress_container.Visibility(
                  xaml::Visibility::Collapsed);
            if (self->xaml_launch_btn)
              self->xaml_launch_btn.Visibility(xaml::Visibility::Visible);
            if (self->xaml_cancel_install_btn)
              self->xaml_cancel_install_btn.Visibility(
                  xaml::Visibility::Collapsed);
          }
        } else {
          InvalidateRect(hwnd, nullptr, FALSE);
        }
      }
      return 0;
    }

    case kWorkerFinished:
      if (self && self->worker_thread.joinable()) {
        self->worker_thread.join();
        KillTimer(hwnd, 1);
        if (self->close_requested)
          PostQuitMessage(self->exit_code);
      }
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd, 1);
      return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  void RenderCaptionAndFallback(HDC hdc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    // Clear the entire host, including the area visible through the XAML
    // island.
    int paint_height = height;
    if (width <= 0 || paint_height <= 0)
      return;

    HDC hdcMem = CreateCompatibleDC(hdc);
    if (!hdcMem)
      return;
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -paint_height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    void *pixels = nullptr;
    HBITMAP hbmMem = CreateDIBSection(hdc, &bitmap_info, DIB_RGB_COLORS,
                                      &pixels, nullptr, 0);
    if (!hbmMem) {
      DeleteDC(hdcMem);
      return;
    }
    HGDIOBJ oldBmp = SelectObject(hdcMem, hbmMem);

    {
      // Preserve premultiplied alpha when copying the buffer into the DWM
      // frame. An uninitialized compatible bitmap leaves opaque pixels and
      // stale text.
      Gdiplus::Bitmap buffer(width, paint_height, width * 4,
                             PixelFormat32bppPARGB,
                             static_cast<BYTE *>(pixels));
      Gdiplus::Graphics g(&buffer);
      g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
      g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

      const bool transparent_background =
          is_mica_active && (!is_xaml_active || has_xaml_transparency);
      g.Clear(transparent_background
                  ? Gdiplus::Color(0, 0, 0, 0)
                  : Gdiplus::Color(255, palette.window_bg.R,
                                   palette.window_bg.G, palette.window_bg.B));

      if (!is_xaml_active) {
        if (caption_bitmap) {
          g.DrawImage(caption_bitmap.get(), 12.0f * dpi_scale, 8.0f * dpi_scale,
                      16.0f * dpi_scale, 16.0f * dpi_scale);
        }
        Gdiplus::Font caption_font(L"Segoe UI", 13.0f * dpi_scale,
                                   Gdiplus::FontStyleRegular,
                                   Gdiplus::UnitPixel);
        Gdiplus::SolidBrush text(Gdiplus::Color(255, 9, 9, 11));
        g.DrawString(is_uninstall ? L"Krypton Uninstall" : L"Krypton Setup", -1,
                     &caption_font,
                     Gdiplus::PointF(36.0f * dpi_scale, 7.0f * dpi_scale),
                     &text);
        Gdiplus::SolidBrush min_hover(Gdiplus::Color(24, 0, 0, 0));
        Gdiplus::SolidBrush close_hover(Gdiplus::Color(255, 196, 43, 28));
        if (min_hovered)
          g.FillRectangle(&min_hover, min_btn_rect);
        if (close_hovered)
          g.FillRectangle(&close_hover, close_btn_rect);
        Gdiplus::Pen glyph(Gdiplus::Color(255, 0, 0, 0), dpi_scale);
        const float my = min_btn_rect.Height / 2.0f;
        const float mx = min_btn_rect.X + min_btn_rect.Width / 2.0f;
        g.DrawLine(&glyph, mx - 5.0f * dpi_scale, my, mx + 5.0f * dpi_scale,
                   my);
        if (close_hovered)
          glyph.SetColor(Gdiplus::Color(255, 255, 255, 255));
        const float cx = close_btn_rect.X + close_btn_rect.Width / 2.0f;
        const float cy = close_btn_rect.Height / 2.0f;
        const float radius = 4.0f * dpi_scale;
        g.DrawLine(&glyph, cx - radius, cy - radius, cx + radius, cy + radius);
        g.DrawLine(&glyph, cx + radius, cy - radius, cx - radius, cy + radius);
      }

      // 2. If System XAML is inactive, render GDI+ fallback view
      if (!is_xaml_active) {
        float s_card = 128.0f * dpi_scale;
        float s_x = (static_cast<float>(width) - s_card) / 2.0f;
        float s_y = (kCaptionHeight + 32.0f) * dpi_scale;

        // Ambient Shadow under Logo Card
        for (int i = 4; i >= 1; --i) {
          Gdiplus::RectF s_rect(
              s_x - i * 1.5f * dpi_scale, s_y + i * 2.0f * dpi_scale,
              s_card + i * 3.0f * dpi_scale, s_card + i * 3.0f * dpi_scale);
          Gdiplus::GraphicsPath s_path;
          AddRoundedRect(s_path, s_rect, 20.0f * dpi_scale);
          Gdiplus::SolidBrush s_brush(
              Gdiplus::Color(static_cast<BYTE>(4 * i), 0, 0, 0));
          g.FillPath(&s_brush, &s_path);
        }

        // 128x128 Brand Card
        Gdiplus::RectF logo_rect(s_x, s_y, s_card, s_card);
        Gdiplus::GraphicsPath logo_path;
        AddRoundedRect(logo_path, logo_rect, 20.0f * dpi_scale);

        Gdiplus::SolidBrush card_bg(
            Gdiplus::Color(palette.card_bg.A, palette.card_bg.R,
                           palette.card_bg.G, palette.card_bg.B));
        g.FillPath(&card_bg, &logo_path);

        Gdiplus::Pen card_border(
            Gdiplus::Color(palette.card_border.A, palette.card_border.R,
                           palette.card_border.G, palette.card_border.B),
            1.0f * dpi_scale);
        g.DrawPath(&card_border, &logo_path);

        // Official Product Logo inside card
        if (logo_bitmap) {
          float img_size = 100.0f * dpi_scale;
          float img_x = s_x + (s_card - img_size) / 2.0f;
          float img_y = s_y + (s_card - img_size) / 2.0f;
          g.DrawImage(logo_bitmap.get(), img_x, img_y, img_size, img_size);
        }

        std::wstring headline;
        std::wstring stage;
        {
          std::lock_guard lock(ui_mutex);
          headline = headline_text;
          stage = stage_text;
        }
        // Headline Text
        float text_top = s_y + s_card + (20.0f * dpi_scale);
        Gdiplus::Font h_font(L"Segoe UI Semibold", 26.0f * dpi_scale,
                             Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush text_dark(
            Gdiplus::Color(255, palette.text_primary.R, palette.text_primary.G,
                           palette.text_primary.B));
        Gdiplus::RectF h_rect(0.0f, text_top, static_cast<float>(width),
                              34.0f * dpi_scale);
        Gdiplus::StringFormat center_fmt;
        center_fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(headline.c_str(), -1, &h_font, h_rect, &center_fmt,
                     &text_dark);

        // Stage Subtitle
        float sub_top = text_top + (40.0f * dpi_scale);
        Gdiplus::Font s_font(L"Segoe UI Variable Text", 14.0f * dpi_scale,
                             Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush text_muted(
            Gdiplus::Color(255, palette.text_secondary.R,
                           palette.text_secondary.G, palette.text_secondary.B));
        Gdiplus::RectF s_rect_box(
            24.0f * dpi_scale, sub_top,
            static_cast<float>(width) - (48.0f * dpi_scale), 36.0f * dpi_scale);
        g.DrawString(stage.c_str(), -1, &s_font, s_rect_box, &center_fmt,
                     &text_muted);

        if (!is_started) {
          // "Unlock the Experience" Pill Button
          Gdiplus::GraphicsPath unl_path;
          AddRoundedRect(unl_path, unlock_btn_rect, 10.0f * dpi_scale);
          Gdiplus::Color unl_color =
              unlock_pressed
                  ? Gdiplus::Color(255, palette.btn_primary_pressed.R,
                                   palette.btn_primary_pressed.G,
                                   palette.btn_primary_pressed.B)
              : unlock_hovered
                  ? Gdiplus::Color(255, palette.btn_primary_hover.R,
                                   palette.btn_primary_hover.G,
                                   palette.btn_primary_hover.B)
                  : Gdiplus::Color(255, palette.btn_primary_bg.R,
                                   palette.btn_primary_bg.G,
                                   palette.btn_primary_bg.B);
          Gdiplus::SolidBrush unl_brush(unl_color);
          g.FillPath(&unl_brush, &unl_path);

          Gdiplus::Font btn_font(L"Segoe UI Semibold", 14.0f * dpi_scale,
                                 Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
          Gdiplus::SolidBrush white_brush(Gdiplus::Color(255, 255, 255, 255));
          center_fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
          g.DrawString(
              (is_uninstall ? L"Uninstall Krypton" : L"Unlock the Experience"),
              -1, &btn_font, unlock_btn_rect, &center_fmt, &white_brush);

          // "Advanced options" Link
          Gdiplus::Font adv_font(L"Segoe UI Variable Text", 13.0f * dpi_scale,
                                 Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
          Gdiplus::SolidBrush adv_brush(
              adv_hovered
                  ? Gdiplus::Color(255, palette.link_hover_fg.R,
                                   palette.link_hover_fg.G,
                                   palette.link_hover_fg.B)
                  : Gdiplus::Color(255, palette.link_fg.R, palette.link_fg.G,
                                   palette.link_fg.B));
          if (!is_uninstall)
            g.DrawString(L"Advanced options", -1, &adv_font, adv_btn_rect,
                         &center_fmt, &adv_brush);
        } else if (is_completed) {
          // "Launch Krypton" Pill Button
          Gdiplus::GraphicsPath lau_path;
          AddRoundedRect(lau_path, launch_btn_rect, 10.0f * dpi_scale);
          Gdiplus::Color lau_color =
              launch_pressed   ? Gdiplus::Color(255, 5, 150, 105)
              : launch_hovered ? Gdiplus::Color(255, 52, 211, 153)
                               : Gdiplus::Color(255, 16, 185, 129);
          Gdiplus::SolidBrush lau_brush(lau_color);
          g.FillPath(&lau_brush, &lau_path);

          Gdiplus::Font btn_font(L"Segoe UI Semibold", 14.0f * dpi_scale,
                                 Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
          Gdiplus::SolidBrush white_brush(Gdiplus::Color(255, 255, 255, 255));
          center_fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
          g.DrawString((is_uninstall ? L"Done" : L"Launch Krypton \u2192"), -1,
                       &btn_font, launch_btn_rect, &center_fmt, &white_brush);
        } else {
          // Layered Progress Bar
          float pb_w = 256.0f * dpi_scale;
          float pb_x = (static_cast<float>(width) - pb_w) / 2.0f;
          float pb_y = sub_top + (42.0f * dpi_scale);
          Gdiplus::RectF baseline_rc(pb_x, pb_y + (1.0f * dpi_scale), pb_w,
                                     2.0f * dpi_scale);
          Gdiplus::SolidBrush base_brush(Gdiplus::Color(
              255, palette.progress_baseline.R, palette.progress_baseline.G,
              palette.progress_baseline.B));
          Gdiplus::GraphicsPath base_path;
          AddRoundedRect(base_path, baseline_rc, 1.0f * dpi_scale);
          g.FillPath(&base_brush, &base_path);

          float fill_w = std::clamp(animated_progress, 0.0f, 1.0f) * pb_w;
          if (fill_w > 2.0f * dpi_scale) {
            Gdiplus::RectF fill_rc(pb_x, pb_y, fill_w, 4.0f * dpi_scale);
            Gdiplus::SolidBrush fill_brush(Gdiplus::Color(
                255, palette.progress_fill.R, palette.progress_fill.G,
                palette.progress_fill.B));
            Gdiplus::GraphicsPath fill_path;
            AddRoundedRect(fill_path, fill_rc, 2.0f * dpi_scale);
            g.FillPath(&fill_brush, &fill_path);
          }
          Gdiplus::Font cancel_font(L"Segoe UI", 13.0f * dpi_scale,
                                    Gdiplus::FontStyleRegular,
                                    Gdiplus::UnitPixel);
          Gdiplus::SolidBrush cancel_brush(Gdiplus::Color(
              255, palette.link_fg.R, palette.link_fg.G, palette.link_fg.B));
          center_fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
          g.DrawString(has_error ? L"Close" : L"Cancel installation", -1,
                       &cancel_font, adv_btn_rect, &center_fmt, &cancel_brush);
        }
      }
    }

    BitBlt(hdc, 0, 0, width, paint_height, hdcMem, 0, 0, SRCCOPY);
    SelectObject(hdcMem, oldBmp);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
  }
};

WinUIInstallerWindow::WinUIInstallerWindow() : impl_(std::make_unique<Impl>()) {
  Gdiplus::GdiplusStartupInput input;
  Gdiplus::GdiplusStartup(&impl_->gdiplus_token, &input, nullptr);
  impl_->LoadGdiplusLogo();
}

WinUIInstallerWindow::~WinUIInstallerWindow() {
  if (impl_->worker_thread.joinable()) {
    impl_->worker_thread.join();
  }
  if (impl_->engine)
    impl_->engine->SetObserver(nullptr);
  impl_->ReleaseXaml();
  if (impl_->apartment_initialized)
    winrt::uninit_apartment();
  impl_->logo_bitmap.reset();
  impl_->caption_bitmap.reset();
  if (impl_->app_icon_small) {
    DestroyIcon(impl_->app_icon_small);
  }
  if (impl_->gdiplus_token) {
    Gdiplus::GdiplusShutdown(impl_->gdiplus_token);
  }
}

void WinUIInstallerWindow::Initialize(UpdaterEngine *engine) {
  impl_->engine = engine;
  if (engine) {
    impl_->is_uninstall = engine->Options().uninstall_mode;
    if (impl_->is_uninstall) {
      impl_->headline_text = L"Sad to see you go";
      impl_->stage_text = L"Thank you for trying Krypton.";
    }
    engine->SetObserver(this);
  }
}

int WinUIInstallerWindow::RunModal() {
  const auto compatibility =
      EvaluateBrowserCompatibility(QuerySystemCapabilities());
  if (!compatibility && !impl_->is_uninstall) {
    const auto message = CompatibilityErrorMessage(compatibility.error());
    const std::wstring wide(message.begin(), message.end());
    MessageBoxW(nullptr, wide.c_str(), L"Krypton Setup - Unsupported System",
                MB_ICONINFORMATION | MB_OK);
    return 1;
  }

  HINSTANCE hInst = GetModuleHandleW(nullptr);

  const UINT sys_dpi = GetDpiForSystem();
  impl_->UpdateDpiMetrics(sys_dpi);
  HICON app_icon_big = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON));

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = Impl::WndProc;
  wc.hInstance = hInst;
  wc.lpszClassName = L"KryptonModernInstallerHost";
  wc.hIcon = app_icon_big;
  wc.hIconSm = impl_->app_icon_small;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  RegisterClassExW(&wc);

  const int screen_w = GetSystemMetrics(SM_CXSCREEN);
  const int screen_h = GetSystemMetrics(SM_CYSCREEN);
  const auto outer = impl_->OuterSize(sys_dpi);
  HWND hwnd = CreateWindowExW(
      WS_EX_APPWINDOW, wc.lpszClassName,
      impl_->is_uninstall ? L"Krypton Uninstall" : L"Krypton Setup",
      Impl::kWindowStyle, (screen_w - outer.cx) / 2, (screen_h - outer.cy) / 2,
      outer.cx, outer.cy, nullptr, nullptr, hInst, impl_.get());
  if (!hwnd)
    return 1;

  const UINT win_dpi = GetDpiForWindow(hwnd);
  if (win_dpi && win_dpi != sys_dpi) {
    impl_->UpdateDpiMetrics(win_dpi);
    const auto monitor_outer = impl_->OuterSize(win_dpi);
    SetWindowPos(hwnd, nullptr, (screen_w - monitor_outer.cx) / 2,
                 (screen_h - monitor_outer.cy) / 2, monitor_outer.cx,
                 monitor_outer.cy, SWP_NOZORDER | SWP_NOACTIVATE);
  }

  // Apply Mica backdrop and Immersive Dark Mode to HWND
  impl_->is_mica_active = ApplyMicaBackdrop(hwnd);
  impl_->palette = ThemePalette::Create();

  // Attempt to initialize in-box System XAML Island hosting (Layer 2
  // Resiliency)
  try {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    impl_->apartment_initialized = true;
  } catch (const winrt::hresult_error &) {
    // The native fallback remains available without XAML.
  }
  if (impl_->apartment_initialized && IsSystemXamlSupported()) {
    try {
      impl_->xaml_manager =
          hosting::WindowsXamlManager::InitializeForCurrentThread();
      impl_->xaml_source = hosting::DesktopWindowXamlSource();

      auto interop = impl_->xaml_source.as<IDesktopWindowXamlSourceNative>();
      winrt::check_hresult(interop->AttachToWindow(hwnd));

      // Enable background transparency for XAML Islands so DWM Acrylic / Mica
      // bleeds through IXamlSourceTransparency is exposed by the thread's XAML
      // Window, not the source.
      auto cur_win = xaml::Window::Current();
      if (cur_win) {
        winrt::com_ptr<IXamlSourceTransparency> trans;
        if (SUCCEEDED(winrt::get_unknown(cur_win)->QueryInterface(
                __uuidof(IXamlSourceTransparency), trans.put_void()))) {
          impl_->has_xaml_transparency =
              SUCCEEDED(trans->put_IsBackgroundTransparent(true));
        }
      }

      HWND hXaml = nullptr;
      interop->get_WindowHandle(&hXaml);
      impl_->xaml_hwnd = hXaml;

      // One XAML root paints the caption and body over the same Acrylic brush.
      SetWindowPos(hXaml, nullptr, 0, 0, impl_->phys_width, impl_->phys_height,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

      impl_->CreateSystemXamlUI();
    } catch (...) {
      impl_->is_xaml_active = false;
      if (impl_->xaml_hwnd)
        ShowWindow(impl_->xaml_hwnd, SW_HIDE);
    }
  }

  // Repaint after XAML initialization chooses the transparent or solid fallback
  // path.
  InvalidateRect(hwnd, nullptr, FALSE);
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);

  MSG msg{};
  BOOL message_result;
  while ((message_result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
    // Keep native host commands local; forward island input and keyboard
    // navigation.
    const bool keyboard_message =
        msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST;
    if (impl_->is_xaml_active && impl_->xaml_source &&
        (msg.hwnd != hwnd || keyboard_message)) {
      auto interop2 =
          impl_->xaml_source.try_as<IDesktopWindowXamlSourceNative2>();
      if (interop2) {
        BOOL processed = FALSE;
        interop2->PreTranslateMessage(&msg, &processed);
        if (processed)
          continue;
      }
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  if (impl_->worker_thread.joinable()) {
    if (impl_->engine)
      impl_->engine->Cancel();
    impl_->worker_thread.join();
  }
  impl_->ReleaseXaml();
  DestroyWindow(hwnd);
  return message_result < 0 ? 1 : impl_->exit_code;
}

void WinUIInstallerWindow::SetResumed(bool resumed) {
  std::lock_guard lock(impl_->ui_mutex);
  impl_->is_resumed = resumed;
}

void WinUIInstallerWindow::OnStateChanged(UpdateState state) {
  {
    std::lock_guard lock(impl_->ui_mutex);
    impl_->current_state = state;
    if (state == UpdateState::kCompleted) {
      impl_->is_completed = true;
      impl_->headline_text =
          impl_->is_uninstall ? L"Krypton removed" : L"Installation Complete";
      impl_->stage_text = impl_->is_uninstall
                              ? L"Thank you for trying Krypton."
                              : L"The internet looks better from here.";
      impl_->target_progress = 1.0f;
    } else if (state == UpdateState::kError) {
      impl_->headline_text =
          impl_->is_uninstall ? L"Uninstall failed" : L"Installation Failed";
      if (!impl_->has_error)
        impl_->stage_text = L"The operation could not be completed.";
      impl_->has_error = true;
    } else if (impl_->is_uninstall) {
      impl_->headline_text = L"Uninstalling Krypton";
      impl_->stage_text = L"Your browsing data will be kept.";
    } else {
      if (state == UpdateState::kVerifying)
        impl_->headline_text = L"Checking installation files";
      else if (state == UpdateState::kExtracting ||
               state == UpdateState::kInstalling)
        impl_->headline_text = L"Installing Krypton";
      else if (state == UpdateState::kDownloading)
        impl_->headline_text = L"Downloading Krypton";
      impl_->stage_text =
          GetDynamicStageText(state, impl_->target_progress, impl_->is_resumed);
    }
  }
  impl_->NotifyStatus();
}

void WinUIInstallerWindow::OnProgress(const ProgressReport &progress) {
  {
    std::lock_guard lock(impl_->ui_mutex);
    impl_->target_progress = progress.fraction;
    if (progress.status_detail.find("resumed") != std::string::npos ||
        progress.status_detail.find("Resumed") != std::string::npos)
      impl_->is_resumed = true;
    impl_->stage_text =
        impl_->is_uninstall
            ? L"Your browsing data will be kept."
            : GetDynamicStageText(impl_->current_state, progress.fraction,
                                  impl_->is_resumed);
  }
  impl_->NotifyStatus();
}

void WinUIInstallerWindow::OnSecurityAlert(const SecurityAlert &alert) {
  {
    std::lock_guard lock(impl_->ui_mutex);
    impl_->stage_text =
        std::format(L"Security Alert: {}",
                    std::wstring(alert.reason.begin(), alert.reason.end()));
  }
  impl_->NotifyStatus();
}

void WinUIInstallerWindow::OnError(std::string_view message) {
  {
    std::lock_guard lock(impl_->ui_mutex);
    impl_->has_error = true;
    impl_->headline_text = L"Error";
    impl_->stage_text =
        std::format(L"{}", std::wstring(message.begin(), message.end()));
  }
  impl_->NotifyStatus();
}

} // namespace kr::updater::windows
