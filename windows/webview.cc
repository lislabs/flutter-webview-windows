#include "webview.h"

#include <atlstr.h>
#include <fmt/core.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.UI.Core.h>

#include <iostream>

#include "webview_host.h"

namespace {
auto CreateDesktopWindowTarget(
    winrt::Windows::UI::Composition::Compositor const& compositor,
    HWND window) {
  namespace abi = ABI::Windows::UI::Composition::Desktop;

  auto interop = compositor.as<abi::ICompositorDesktopInterop>();
  winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget target{nullptr};
  winrt::check_hresult(interop->CreateDesktopWindowTarget(
      window, true,
      reinterpret_cast<abi::IDesktopWindowTarget**>(winrt::put_abi(target))));
  return target;
}

inline auto towstring(std::string_view str) {
  return std::wstring(str.begin(), str.end());
}

}  // namespace

Webview::Webview(
    wil::com_ptr<ICoreWebView2CompositionController> composition_controller,
    WebviewHost* host, HWND hwnd, bool owns_window, bool offscreen_only)
    : composition_controller_(std::move(composition_controller)),
      host_(host),
      hwnd_(hwnd),
      owns_window_(owns_window) {
  webview_controller_ =
      composition_controller_.query<ICoreWebView2Controller3>();
  webview_controller_->get_CoreWebView2(webview_.put());

  webview_controller_->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS);
  webview_controller_->put_ShouldDetectMonitorScaleChanges(FALSE);
  webview_controller_->put_RasterizationScale(1.0);

  wil::com_ptr<ICoreWebView2Settings> settings;
  if (webview_->get_Settings(settings.put()) == S_OK) {
    settings2_ = settings.try_query<ICoreWebView2Settings2>();

    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_AreDefaultContextMenusEnabled(FALSE);
  }

  RegisterEventHandlers();

  auto compositor = host->compositor();
  auto root = compositor.CreateContainerVisual();

  // initial size. doesn't matter as we resize the surface anyway.
  root.Size({1280, 720});
  root.IsVisible(true);
  surface_ = root.as<winrt::Windows::UI::Composition::Visual>();

  // Create on-screen window for debugging purposes
  if (!offscreen_only) {
    window_target_ = CreateDesktopWindowTarget(compositor, hwnd);
    window_target_.Root(root);
  }

  auto webview_visual = compositor.CreateSpriteVisual();
  webview_visual.RelativeSizeAdjustment({1.0f, 1.0f});

  root.Children().InsertAtTop(webview_visual);

  composition_controller_->put_RootVisualTarget(
      webview_visual.as<IUnknown>().get());

  webview_controller_->put_IsVisible(true);
}

Webview::~Webview() {
  if (owns_window_) {
    DestroyWindow(hwnd_);
  }
}

void Webview::RegisterEventHandlers() {
  webview_->add_ContentLoading(
      Callback<ICoreWebView2ContentLoadingEventHandler>(
          [this](ICoreWebView2* sender, IUnknown* args) -> HRESULT {
            if (loading_state_changed_callback_) {
              loading_state_changed_callback_(WebviewLoadingState::Loading);
            }

            return S_OK;
          })
          .Get(),
      &content_loading_token_);

  webview_->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [this](ICoreWebView2* sender, IUnknown* args) -> HRESULT {
            if (loading_state_changed_callback_) {
              loading_state_changed_callback_(
                  WebviewLoadingState::NavigationCompleted);
            }

            return S_OK;
          })
          .Get(),
      &navigation_completed_token_);

  webview_->add_SourceChanged(
      Callback<ICoreWebView2SourceChangedEventHandler>(
          [this](ICoreWebView2* sender, IUnknown* args) -> HRESULT {
            LPWSTR wurl;
            if (url_changed_callback_ && webview_->get_Source(&wurl) == S_OK) {
              std::string url = CW2A(wurl);
              url_changed_callback_(url);
            }

            return S_OK;
          })
          .Get(),
      &source_changed_token_);

  webview_->add_DocumentTitleChanged(
      Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
          [this](ICoreWebView2* sender, IUnknown* args) -> HRESULT {
            LPWSTR wtitle;
            if (document_title_changed_callback_ &&
                webview_->get_DocumentTitle(&wtitle) == S_OK) {
              std::string title = CW2A(wtitle);
              document_title_changed_callback_(title);
            }

            return S_OK;
          })
          .Get(),
      &document_title_changed_token_);

  composition_controller_->add_CursorChanged(
      Callback<ICoreWebView2CursorChangedEventHandler>(
          [this](ICoreWebView2CompositionController* sender,
                 IUnknown* args) -> HRESULT {
            HCURSOR cursor;
            if (cursor_changed_callback_ &&
                sender->get_Cursor(&cursor) == S_OK) {
              cursor_changed_callback_(cursor);
            }
            return S_OK;
          })
          .Get(),
      &cursor_changed_token_);

  webview_controller_->add_GotFocus(
      Callback<ICoreWebView2FocusChangedEventHandler>(
          [this](ICoreWebView2Controller* sender, IUnknown* args) -> HRESULT {
            if (focus_changed_callback_) {
              focus_changed_callback_(true);
            }
            return S_OK;
          })
          .Get(),
      &got_focus_token_);

  webview_controller_->add_LostFocus(
      Callback<ICoreWebView2FocusChangedEventHandler>(
          [this](ICoreWebView2Controller* sender, IUnknown* args) -> HRESULT {
            if (focus_changed_callback_) {
              focus_changed_callback_(false);
            }
            return S_OK;
          })
          .Get(),
      &lost_focus_token_);
}

void Webview::SetSurfaceSize(size_t width, size_t height) {
  auto surface = surface_.get();

  if (surface) {
    surface.Size({(float)width, (float)height});

    RECT bounds;
    bounds.left = 0;
    bounds.top = 0;
    bounds.right = static_cast<LONG>(width);
    bounds.bottom = static_cast<LONG>(height);

    if (webview_controller_->put_Bounds(bounds) != S_OK) {
      std::cerr << "Setting webview bounds failed." << std::endl;
    }

    if (surface_size_changed_callback_) {
      surface_size_changed_callback_(width, height);
    }
  }
}

bool Webview::ClearCookies() {
  return webview_->CallDevToolsProtocolMethod(L"Network.clearBrowserCookies",
                                              L"{}", nullptr) == S_OK;
}

bool Webview::SetUserAgent(const std::string& user_agent) {
  if (settings2_) {
    return settings2_->put_UserAgent(towstring(user_agent).c_str()) == S_OK;
  }
  return false;
}

void Webview::SetCursorPos(double x, double y) {
  POINT point;
  point.x = static_cast<LONG>(x);
  point.y = static_cast<LONG>(y);
  last_cursor_pos_ = point;

  // https://docs.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2?view=webview2-1.0.774.44
  composition_controller_->SendMouseInput(
      COREWEBVIEW2_MOUSE_EVENT_KIND::COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE,
      virtual_keys_.state(), 0, point);
}

void Webview::SetPointerButtonState(WebviewPointerButton button, bool is_down) {
  COREWEBVIEW2_MOUSE_EVENT_KIND kind;
  switch (button) {
    case WebviewPointerButton::Primary:
      virtual_keys_.set_isLeftButtonDown(is_down);
      kind = is_down ? COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN
                     : COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
      break;
    case WebviewPointerButton::Secondary:
      virtual_keys_.set_isRightButtonDown(is_down);
      kind = is_down ? COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN
                     : COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
      break;
    case WebviewPointerButton::Tertiary:
      virtual_keys_.set_isMiddleButtonDown(is_down);
      kind = is_down ? COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN
                     : COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
      break;
    default:
      kind = static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(0);
  }

  composition_controller_->SendMouseInput(kind, virtual_keys_.state(), 0,
                                          last_cursor_pos_);
}

void Webview::SendScroll(double delta, bool horizontal) {
  // delta * 6 gives me a multiple of WHEEL_DELTA (120)
  constexpr auto kScrollMultiplier = 6;

  auto offset = static_cast<short>(delta * kScrollMultiplier);

  // TODO Remove this workaround
  //
  // For some reason, the composition controller only handles mousewheel events
  // if a mouse button is down.
  // -> Emulate a down button while sending the wheel event (a virtual key
  //    doesn't work)
  composition_controller_->SendMouseInput(
      COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOWN,
      COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0, last_cursor_pos_);

  if (horizontal) {
    composition_controller_->SendMouseInput(
        COREWEBVIEW2_MOUSE_EVENT_KIND_HORIZONTAL_WHEEL, virtual_keys_.state(),
        offset, last_cursor_pos_);
  } else {
    composition_controller_->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL,
                                            virtual_keys_.state(), offset,
                                            last_cursor_pos_);
  }

  composition_controller_->SendMouseInput(
      COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP,
      COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0, last_cursor_pos_);
}

void Webview::SetScrollDelta(double delta_x, double delta_y) {
  if (delta_x != 0.0) {
    SendScroll(delta_x, true);
  }
  if (delta_y != 0.0) {
    SendScroll(delta_y, false);
  }
}

void Webview::LoadUrl(const std::string& url) {
  webview_->Navigate(towstring(url).c_str());
}

void Webview::LoadStringContent(const std::string& content) {
  webview_->NavigateToString(towstring(content).c_str());
}

void Webview::Reload() { webview_->Reload(); }
