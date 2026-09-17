// Implements the modal search window as a fully custom D3D11/D2D1 UI.
// There are no native edit, radio, combo-box, or button child windows. This
// permits identical light/dark/high-contrast rendering and predictable DPI
// scaling while retaining keyboard, clipboard, and Unicode text input support.

#include "SearchDialog.h"

#include "DialogSurface.h"
#include "Security.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace {
constexpr std::size_t kMaximumQueryCharacters = 64u * 1024u;
constexpr wchar_t kSearchWindowClass[] = L"BinEdit.SearchDialog";
// Private posted message used to coalesce hover, caret, and input invalidations.
constexpr UINT WM_SEARCH_RENDER = WM_APP + 42;
constexpr UINT_PTR kCaretTimer = 1;

// Keyboard focus is logical because the dialog intentionally contains no child HWNDs.
enum class FocusItem { Query, Binary, Text, Encoding, Find, Cancel };
// Hover values distinguish the four custom encoding chips for direct hit testing.
enum class HoverItem { None, Query, Binary, Text, Encoding0, Encoding1, Encoding2, Encoding3, Find, Cancel };

// All geometry is derived from client size and DPI on demand. No cached rectangles
// can become stale after WM_SIZE or WM_DPICHANGED.
struct SearchLayout {
    D2D1_RECT_F queryLabel{}, query{}, binary{}, text{}, encodingLabel{};
    std::array<D2D1_RECT_F, 4> encodings{};
    D2D1_RECT_F error{}, find{}, cancel{};
};

bool Contains(D2D1_RECT_F rect, float x, float y) { return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom; }

// Produces deterministic hover colors without introducing alpha-composition state.
std::uint32_t Mix(std::uint32_t a, std::uint32_t b, unsigned bAmount) {
    const unsigned aAmount = 255 - bAmount;
    const auto channel = [=](unsigned shift) { return ((((a >> shift) & 0xff) * aAmount + ((b >> shift) & 0xff) * bAmount) / 255) << shift; };
    return channel(16) | channel(8) | channel(0);
}

class SearchWindow {
public:
    SearchWindow(HWND owner, HINSTANCE instance, const SearchDialogResult& initial, const Profile& profile)
        : owner_(owner), instance_(instance), result_(initial), profile_(&profile), palette_(profile.ResolvePalette()),
          strings_(&GetStrings(initial.language)), cursor_(initial.query.size()) {
        // Acceptance is output state and must not leak from the previous opening.
        result_.accepted = false;
    }

    SearchDialogResult Show() {
        // Size is specified in 96-DPI logical pixels and converted before window
        // creation so the initial client area is correct on any monitor.
        Register();
        const UINT dpi = owner_ ? GetDpiForWindow(owner_) : 96;
        const float scale = dpi / 96.0f;
        RECT rect{0, 0, static_cast<LONG>(600 * scale), static_cast<LONG>(344 * scale)};
        constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
        AdjustWindowRectExForDpi(&rect, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
        const int width = rect.right - rect.left, height = rect.bottom - rect.top;
        // Center inside the owner's monitor work area and clamp for small screens.
        const DialogPlacement placement = ResolveDialogPlacement(owner_);
        const int x = std::clamp(placement.anchor.left +
            ((placement.anchor.right - placement.anchor.left) - width) / 2,
            placement.workArea.left, std::max(placement.workArea.left, placement.workArea.right - width));
        const int y = std::clamp(placement.anchor.top +
            ((placement.anchor.bottom - placement.anchor.top) - height) / 2,
            placement.workArea.top, std::max(placement.workArea.top, placement.workArea.bottom - height));
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kSearchWindowClass, strings_->searchTitle, style,
            x, y, width, height, owner_, nullptr, instance_, this);
        if (!window_) return result_;
        if (!surface_.Initialize(window_)) { DestroyWindow(window_); return result_; }
        ready_ = true;
        RefreshTheme();
        // Win32 modality is implemented explicitly: disable the owner, run a
        // nested UI-thread loop, then restore activation before returning.
        EnableWindow(owner_, FALSE);
        ShowWindow(window_, IsWindowVisible(owner_) ? SW_SHOW : SW_HIDE);
        SetForegroundWindow(window_);
        SetFocus(window_);
        SetTimer(window_, kCaretTimer, GetCaretBlinkTime(), nullptr);
        RequestDraw();

        MSG message{};
        BOOL messageStatus = 1;
        while (running_) {
            messageStatus = GetMessageW(&message, nullptr, 0, 0);
            if (messageStatus <= 0) break;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        // Preserve a process-wide quit request observed by the nested loop.
        if (window_) DestroyWindow(window_);
        if (messageStatus == 0) PostQuitMessage(static_cast<int>(message.wParam));
        if (owner_ && IsWindow(owner_)) { EnableWindow(owner_, TRUE); SetActiveWindow(owner_); }
        return result_;
    }

private:
    void Register() const {
        // RegisterClassEx is idempotent for this process; ERROR_CLASS_ALREADY_EXISTS
        // is harmless and the subsequent CreateWindowEx remains valid.
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WindowProc; wc.hInstance = instance_; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kSearchWindowClass; wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wc);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        try {
            // The modal Show() call guarantees this stack controller outlives its HWND.
            auto* self = reinterpret_cast<SearchWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE) {
                self = static_cast<SearchWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
                self->window_ = window;
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
        } catch (...) {
            Security::FailFast(L"SearchWindow::WindowProc");
        }
    }

    SearchLayout Layout() const {
        const float s = surface_.Scale(), w = surface_.Width(), h = surface_.Height(), p = 24 * s;
        SearchLayout layout;
        layout.queryLabel = {p, 12 * s, w - p, 40 * s};
        layout.query = {p, 42 * s, w - p, 82 * s};
        layout.binary = {p, 92 * s, p + 150 * s, 132 * s};
        layout.text = {p + 160 * s, 92 * s, p + 300 * s, 132 * s};
        layout.encodingLabel = {p, 134 * s, w - p, 162 * s};
        // Encoding options divide the available width evenly, keeping all four
        // choices usable under DPI scaling and either language.
        const float gap = 8 * s, optionWidth = (w - p * 2 - gap * 3) / 4;
        for (std::size_t i = 0; i < 4; ++i)
            layout.encodings[i] = {p + i * (optionWidth + gap), 164 * s, p + i * (optionWidth + gap) + optionWidth, 204 * s};
        layout.error = {p, 214 * s, w - p, 254 * s};
        const float buttonWidth = 120 * s, buttonHeight = 40 * s;
        layout.cancel = {w - p - buttonWidth, h - p - buttonHeight, w - p, h - p};
        layout.find = {layout.cancel.left - gap - buttonWidth, layout.cancel.top, layout.cancel.left - gap, layout.cancel.bottom};
        return layout;
    }

    // This is the dialog equivalent of R2PR's "render only when required" rule.
    void RequestDraw() { if (!renderQueued_ && ready_) renderQueued_ = PostMessageW(window_, WM_SEARCH_RENDER, 0, 0) != FALSE; }

    void RefreshTheme() {
        // SetWindowTheme can synchronously send WM_THEMECHANGED; guard the call and
        // resolve high contrast again for live accessibility changes.
        if (applyingTheme_) return;
        applyingTheme_ = true;
        palette_ = profile_->ResolvePalette();
        BOOL dark = palette_.dark && !palette_.highContrast;
        constexpr DWORD immersiveDarkMode = 20;
        DwmSetWindowAttribute(window_, immersiveDarkMode, &dark, sizeof(dark));
        SetWindowTheme(window_, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
        applyingTheme_ = false;
        RequestDraw();
    }

    void DrawRadio(const D2D1_RECT_F& rect, const wchar_t* text, bool checked, bool focused, bool hovered) {
        // The outer ring communicates focus through the caret color. The inner dot
        // communicates checked state independently for keyboard-only operation.
        const float s = surface_.Scale();
        if (hovered) surface_.FillRounded(rect, 4 * s, Mix(palette_.background, palette_.text, palette_.dark ? 20 : 10));
        const D2D1_ELLIPSE circle{{rect.left + 12 * s, (rect.top + rect.bottom) / 2}, 8 * s, 8 * s};
        surface_.Ellipse(circle, checked || focused ? palette_.caret : palette_.grid, focused ? 2.0f * s : std::max(1.0f, s));
        if (checked) surface_.Ellipse({circle.point, 4 * s, 4 * s}, palette_.caret, 1, true);
        surface_.Text(text, {rect.left + 30 * s, rect.top, rect.right, rect.bottom}, palette_.text);
    }

    void DrawButton(const D2D1_RECT_F& rect, const wchar_t* text, bool primary, bool focused, bool hovered) {
        std::uint32_t background = primary ? palette_.selection : palette_.surface;
        if (hovered) background = Mix(background, primary ? palette_.selectionText : palette_.text, palette_.dark ? 22 : 14);
        const float s = surface_.Scale();
        surface_.FillRounded(rect, 4 * s, background);
        const std::uint32_t stroke = focused ? (primary ? palette_.selectionText : palette_.caret) : palette_.grid;
        surface_.StrokeRounded(rect, 4 * s, stroke, focused ? 2.0f * s : std::max(1.0f, s));
        surface_.Text(text, rect, primary ? palette_.selectionText : palette_.text, true, primary);
    }

    void Draw() {
        renderQueued_ = false;
        if (!surface_.Begin(palette_)) return;
        const auto layout = Layout();
        const float s = surface_.Scale();
        surface_.CaptionText(strings_->searchQuery, layout.queryLabel, palette_.text);
        surface_.FillRounded(layout.query, 4 * s, palette_.surface);
        const bool queryFocused = focus_ == FocusItem::Query;
        const std::uint32_t queryStroke = queryFocused ? palette_.caret :
            (hover_ == HoverItem::Query ? Mix(palette_.grid, palette_.text, 48) : palette_.grid);
        surface_.StrokeRounded(layout.query, 4 * s, queryStroke, queryFocused ? 2.0f * s : std::max(1.0f, s));
        const auto inner = D2D1::RectF(layout.query.left + 11 * s, layout.query.top + 1, layout.query.right - 11 * s, layout.query.bottom - 1);
        // Measure shaped text and scroll only enough to keep the insertion point
        // visible. Clipping prevents long queries from covering adjacent controls.
        const float caretPosition = surface_.TextPosition(result_.query, cursor_, 10000 * s);
        const float scroll = std::max(0.0f, caretPosition - (inner.right - inner.left) + 3 * s);
        surface_.PushClip(inner);
        if (selectAll_ && !result_.query.empty()) {
            const float selectedRight = inner.left - scroll + surface_.TextPosition(result_.query, result_.query.size(), 10000 * s);
            surface_.FillRounded({inner.left - scroll, inner.top + 8 * s, selectedRight, inner.bottom - 8 * s},
                2 * s, palette_.selection);
        }
        surface_.Text(result_.query, {inner.left - scroll, inner.top, inner.left - scroll + 10000 * s, inner.bottom}, selectAll_ ? palette_.selectionText : palette_.text);
        if (focus_ == FocusItem::Query && caretVisible_ && !selectAll_) {
            const float x = inner.left + caretPosition - scroll;
            surface_.Line({x, inner.top + 8 * s}, {x, inner.bottom - 8 * s}, palette_.caret, std::max(1.0f, s));
        }
        surface_.PopClip();
        DrawRadio(layout.binary, strings_->searchBinary, result_.binary, focus_ == FocusItem::Binary, hover_ == HoverItem::Binary);
        DrawRadio(layout.text, strings_->searchText, !result_.binary, focus_ == FocusItem::Text, hover_ == HoverItem::Text);
        surface_.CaptionText(strings_->searchEncoding, layout.encodingLabel, result_.binary ? palette_.mutedText : palette_.text);
        // Enum order and this array are kept explicit so display order never
        // accidentally depends on compiler-assigned enum values.
        constexpr std::array<TextEncoding, 4> encodings{TextEncoding::Ascii, TextEncoding::Utf8, TextEncoding::Utf16Le, TextEncoding::ShiftJis};
        for (std::size_t i = 0; i < encodings.size(); ++i) {
            const bool selected = result_.encoding == encodings[i];
            const bool activeSelected = selected && !result_.binary;
            std::uint32_t background = activeSelected ? palette_.selection : palette_.surface;
            if (hover_ == static_cast<HoverItem>(static_cast<int>(HoverItem::Encoding0) + static_cast<int>(i))) background = Mix(background, palette_.caret, 45);
            surface_.FillRounded(layout.encodings[i], 4 * s, background);
            const bool focused = focus_ == FocusItem::Encoding && selected;
            surface_.StrokeRounded(layout.encodings[i], 4 * s,
                focused ? (activeSelected ? palette_.selectionText : palette_.caret) : palette_.grid,
                focused ? 2.0f * s : std::max(1.0f, s));
            surface_.Text(EncodingName(encodings[i]), layout.encodings[i], activeSelected ? palette_.selectionText :
                (result_.binary ? palette_.mutedText : palette_.text), true);
        }
        DrawButton(layout.find, strings_->searchButton, true, focus_ == FocusItem::Find, hover_ == HoverItem::Find);
        DrawButton(layout.cancel, strings_->cancelButton, false, focus_ == FocusItem::Cancel, hover_ == HoverItem::Cancel);
        // Validation stays inside the themed D2D surface rather than opening an
        // unthemed system MessageBox over the custom dialog.
        if (!inlineError_.empty()) {
            const std::uint32_t errorColor = palette_.highContrast ? palette_.text : (palette_.dark ? 0xFF8A80 : 0xB3261E);
            const std::uint32_t errorSurface = palette_.highContrast ? palette_.surface : Mix(palette_.background, errorColor, 18);
            surface_.FillRounded(layout.error, 4 * s, errorSurface);
            surface_.StrokeRounded(layout.error, 4 * s, errorColor, std::max(1.0f, s));
            surface_.Text(inlineError_, {layout.error.left + 12 * s, layout.error.top,
                layout.error.right - 12 * s, layout.error.bottom}, errorColor);
        }
        if (!surface_.End()) RequestDraw();
    }

    HoverItem Hit(float x, float y) const {
        // Hit testing uses the exact rectangles drawn in the current frame.
        const auto layout = Layout();
        if (Contains(layout.query, x, y)) return HoverItem::Query;
        if (Contains(layout.binary, x, y)) return HoverItem::Binary;
        if (Contains(layout.text, x, y)) return HoverItem::Text;
        for (std::size_t i = 0; i < 4; ++i) if (Contains(layout.encodings[i], x, y))
            return static_cast<HoverItem>(static_cast<int>(HoverItem::Encoding0) + static_cast<int>(i));
        if (Contains(layout.find, x, y)) return HoverItem::Find;
        if (Contains(layout.cancel, x, y)) return HoverItem::Cancel;
        return HoverItem::None;
    }

    void Activate(HoverItem item) {
        constexpr std::array<TextEncoding, 4> encodings{TextEncoding::Ascii, TextEncoding::Utf8, TextEncoding::Utf16Le, TextEncoding::ShiftJis};
        if (item == HoverItem::Query) { focus_ = FocusItem::Query; cursor_ = result_.query.size(); selectAll_ = false; }
        else if (item == HoverItem::Binary) SelectMode(true);
        else if (item == HoverItem::Text) SelectMode(false);
        // Selecting an encoding also selects text mode because encoding has no
        // meaning for a binary-pattern search.
        else if (item >= HoverItem::Encoding0 && item <= HoverItem::Encoding3) {
            focus_ = FocusItem::Encoding; result_.binary = false;
            result_.encoding = encodings[static_cast<std::size_t>(static_cast<int>(item) - static_cast<int>(HoverItem::Encoding0))];
        } else if (item == HoverItem::Find) Accept();
        else if (item == HoverItem::Cancel) Close();
        caretVisible_ = true; RequestDraw();
    }

    void SelectMode(bool binary) {
        result_.binary = binary;
        focus_ = binary ? FocusItem::Binary : FocusItem::Text;
        // An error produced under the previous parser no longer describes the
        // current mode. Clear it immediately when the radio selection changes.
        inlineError_.clear();
        caretVisible_ = true;
        RequestDraw();
    }

    bool HandleMnemonic(WPARAM key) {
        switch (key) {
        case 'B': SelectMode(true); return true;
        case 'T': SelectMode(false); return true;
        case 'Q':
            focus_ = FocusItem::Query;
            cursor_ = std::min(cursor_, result_.query.size());
            selectAll_ = false;
            caretVisible_ = true;
            RequestDraw();
            return true;
        default: return false;
        }
    }

    const wchar_t* ErrorText(SearchPatternError error) const {
        switch (error) {
        case SearchPatternError::Empty: return strings_->searchEmpty;
        case SearchPatternError::InvalidHex: return strings_->searchInvalidHex;
        case SearchPatternError::MissingSeparator: return strings_->searchSeparator;
        case SearchPatternError::Unrepresentable: return strings_->searchUnrepresentable;
        case SearchPatternError::EncodingFailed: return strings_->searchEncodingFailed;
        case SearchPatternError::TooLarge: return strings_->searchTooLarge;
        case SearchPatternError::AllocationFailed: return strings_->searchAllocationFailed;
        default: return strings_->searchEncodingFailed;
        }
    }

    void Accept() {
        // Compile once here for themed inline validation. BinEditApp compiles the
        // returned value again as the authoritative search operation.
        SearchPatternError error{};
        const auto pattern = result_.binary ? ParseBinaryPattern(result_.query, error) : EncodeTextPattern(result_.query, result_.encoding, error);
        if (!pattern) { inlineError_ = ErrorText(error); focus_ = FocusItem::Query; RequestDraw(); return; }
        inlineError_.clear(); result_.accepted = true; Close();
    }
    void Close() const { if (window_) DestroyWindow(window_); }

    void InsertText(std::wstring_view text) {
        // WM_CHAR delivers UTF-16 code units after keyboard-layout/IME processing.
        // Insertion therefore operates on std::wstring positions, not raw keys.
        inlineError_.clear();
        if (selectAll_) { result_.query.clear(); cursor_ = 0; selectAll_ = false; }
        const std::size_t available = kMaximumQueryCharacters -
            std::min(kMaximumQueryCharacters, result_.query.size());
        text = text.substr(0, available);
        result_.query.insert(cursor_, text); cursor_ += text.size(); caretVisible_ = true; RequestDraw();
    }

    void CopySelection(bool cut) {
        // The minimal editor currently exposes whole-query selection. Clipboard
        // ownership transfers to Windows only when SetClipboardData succeeds.
        if (!selectAll_ || result_.query.empty() || !OpenClipboard(window_)) return;
        EmptyClipboard();
        const SIZE_T bytes = (result_.query.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            void* data = GlobalLock(memory);
            if (data) { memcpy(data, result_.query.c_str(), bytes); GlobalUnlock(memory); }
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        }
        CloseClipboard();
        if (cut) { result_.query.clear(); cursor_ = 0; selectAll_ = false; RequestDraw(); }
    }

    void Paste() {
        if (!OpenClipboard(window_)) return;
        HANDLE value = GetClipboardData(CF_UNICODETEXT);
        if (value) {
            const auto* text = static_cast<const wchar_t*>(GlobalLock(value));
            if (text) {
                const SIZE_T bytes = GlobalSize(value);
                const std::size_t capacity = bytes / sizeof(wchar_t);
                const wchar_t* end = std::find(text, text + capacity, L'\0');
                if (end != text + capacity) InsertText(std::wstring_view(text, end));
                GlobalUnlock(value);
            }
        }
        CloseClipboard();
    }

    void CycleFocus(bool reverse) {
        // Six logical stops mirror standard dialog Tab and Shift+Tab behavior.
        int value = static_cast<int>(focus_);
        value = (value + (reverse ? 5 : 1)) % 6;
        focus_ = static_cast<FocusItem>(value); selectAll_ = false; caretVisible_ = true; RequestDraw();
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_PAINT: { PAINTSTRUCT paint{}; BeginPaint(window_, &paint); EndPaint(window_, &paint); RequestDraw(); return 0; }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: if (ready_) { surface_.Resize(LOWORD(lParam), HIWORD(lParam)); RequestDraw(); } return 0;
        case WM_DPICHANGED: {
            surface_.DpiChanged(HIWORD(wParam));
            const RECT* rect = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            RequestDraw(); return 0;
        }
        case WM_SEARCH_RENDER: Draw(); return 0;
        case WM_SETTINGCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED: RefreshTheme(); return 0;
        // Caret blinking is the only periodic rendering while the dialog is idle.
        case WM_TIMER: if (wParam == kCaretTimer && focus_ == FocusItem::Query) { caretVisible_ = !caretVisible_; RequestDraw(); } return 0;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0}; TrackMouseEvent(&track);
            const auto item = Hit(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)));
            if (item != hover_) { hover_ = item; RequestDraw(); }
            return 0;
        }
        case WM_MOUSELEAVE: hover_ = HoverItem::None; RequestDraw(); return 0;
        case WM_LBUTTONDOWN: SetFocus(window_); Activate(Hit(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)))); return 0;
        case WM_SYSKEYDOWN:
            // This is a custom D2D window, so USER32 cannot interpret menu-style
            // ampersand markers for us. Preserve familiar Alt+B/Alt+T/Alt+Q
            // access keys explicitly while keeping those markers out of labels.
            if (HandleMnemonic(wParam)) return 0;
            break;
        case WM_SYSCHAR:
            if (wParam == 'b' || wParam == 'B' || wParam == 't' || wParam == 'T' ||
                wParam == 'q' || wParam == 'Q') return 0;
            break;
        case WM_KEYDOWN: {
            // Editing commands live in WM_KEYDOWN; printable Unicode arrives later
            // through WM_CHAR, including characters committed by an IME.
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (wParam == VK_ESCAPE) { Close(); return 0; }
            if (wParam == VK_RETURN) { if (focus_ == FocusItem::Cancel) Close(); else Accept(); return 0; }
            if (wParam == VK_TAB) { CycleFocus(shift); return 0; }
            if (focus_ == FocusItem::Query) {
                if (ctrl && wParam == 'A') { selectAll_ = true; cursor_ = result_.query.size(); RequestDraw(); return 0; }
                if (ctrl && wParam == 'C') { CopySelection(false); return 0; }
                if (ctrl && wParam == 'X') { CopySelection(true); return 0; }
                if (ctrl && wParam == 'V') { Paste(); return 0; }
                if (wParam == VK_LEFT) { selectAll_ = false; if (cursor_) --cursor_; RequestDraw(); return 0; }
                if (wParam == VK_RIGHT) { selectAll_ = false; cursor_ = std::min(result_.query.size(), cursor_ + 1); RequestDraw(); return 0; }
                if (wParam == VK_HOME) { selectAll_ = false; cursor_ = 0; RequestDraw(); return 0; }
                if (wParam == VK_END) { selectAll_ = false; cursor_ = result_.query.size(); RequestDraw(); return 0; }
                if (wParam == VK_DELETE) {
                    inlineError_.clear();
                    if (selectAll_) { result_.query.clear(); cursor_ = 0; selectAll_ = false; }
                    else if (cursor_ < result_.query.size()) result_.query.erase(cursor_, 1);
                    RequestDraw(); return 0;
                }
            } else if ((focus_ == FocusItem::Binary || focus_ == FocusItem::Text) &&
                       (wParam == VK_LEFT || wParam == VK_RIGHT ||
                        wParam == VK_UP || wParam == VK_DOWN)) {
                // A two-item radio group wraps in either direction.
                SelectMode(focus_ == FocusItem::Text);
                return 0;
            } else if (focus_ == FocusItem::Encoding && (wParam == VK_LEFT || wParam == VK_RIGHT)) {
                int current = result_.encoding == TextEncoding::Utf8 ? 1 : (result_.encoding == TextEncoding::Utf16Le ? 2 : (result_.encoding == TextEncoding::ShiftJis ? 3 : 0));
                current = (current + (wParam == VK_LEFT ? 3 : 1)) % 4;
                result_.encoding = static_cast<TextEncoding>(current); result_.binary = false; RequestDraw(); return 0;
            } else if (wParam == VK_SPACE) {
                if (focus_ == FocusItem::Binary) SelectMode(true);
                else if (focus_ == FocusItem::Text) SelectMode(false);
                else if (focus_ == FocusItem::Find) Accept();
                else if (focus_ == FocusItem::Cancel) Close();
                RequestDraw(); return 0;
            }
            break;
        }
        case WM_CHAR:
            if (focus_ == FocusItem::Query) {
                if (wParam == VK_BACK) {
                    inlineError_.clear();
                    if (selectAll_) { result_.query.clear(); cursor_ = 0; selectAll_ = false; }
                    else if (cursor_) result_.query.erase(--cursor_, 1);
                    RequestDraw();
                } else if (wParam >= 0x20) {
                    const wchar_t character = static_cast<wchar_t>(wParam);
                    InsertText(std::wstring_view(&character, 1));
                }
            }
            return 0;
        case WM_CLOSE: Close(); return 0;
        case WM_DESTROY: KillTimer(window_, kCaretTimer); running_ = false; window_ = nullptr; return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    HWND owner_{};
    HINSTANCE instance_{};
    HWND window_{};
    SearchDialogResult result_;
    const Profile* profile_{}; // Borrowed for live palette resolution.
    Palette palette_;
    const UiStrings* strings_{};
    D2DDialogSurface surface_;
    std::size_t cursor_{};
    FocusItem focus_{FocusItem::Query};
    HoverItem hover_{HoverItem::None};
    bool running_{true};
    bool ready_{};
    bool renderQueued_{};
    bool caretVisible_{true};
    bool selectAll_{};
    bool applyingTheme_{};
    std::wstring inlineError_;
};
}

SearchDialogResult ShowSearchDialog(HWND owner, HINSTANCE instance, const SearchDialogResult& initial, const Profile& profile) {
    // Stack lifetime is safe because Show does not return until WM_DESTROY.
    SearchWindow window(owner, instance, initial, profile);
    return window.Show();
}
