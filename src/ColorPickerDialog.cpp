// Implements a fully application-owned HSV/RGB color picker with D3D11, Direct2D,
// and DirectWrite. The dialog owns all visible controls, text editing, clipboard,
// keyboard navigation, pointer dragging, DPI changes, and theme integration.

#include "ColorPickerDialog.h"

#include "ColorMath.h"
#include "DialogSurface.h"
#include "Security.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <string>
#include <string_view>

namespace {
constexpr wchar_t kColorPickerWindowClass[] = L"BinEdit.ColorPickerDialog";
constexpr UINT WM_COLOR_PICKER_RENDER = WM_APP + 45;
constexpr UINT_PTR kCaretTimer = 1;
constexpr std::size_t kFieldCount = 4;

enum class FocusItem { Spectrum, Hue, Red, Green, Blue, Hex, Apply, Cancel };
enum class HoverItem { None, Spectrum, Hue, Red, Green, Blue, Hex, Apply, Cancel };
enum class DragTarget { None, Spectrum, Hue };

struct ColorPickerLayout {
    D2D1_RECT_F spectrumLabel{}, spectrum{}, hue{};
    D2D1_RECT_F currentLabel{}, currentSwatch{}, newLabel{}, newSwatch{};
    std::array<D2D1_RECT_F, 3> rgbLabels{};
    std::array<D2D1_RECT_F, 3> rgbFields{};
    D2D1_RECT_F hexLabel{}, hexField{}, hint{}, error{}, apply{}, cancel{};
};

bool Contains(D2D1_RECT_F rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

std::uint32_t Mix(std::uint32_t a, std::uint32_t b, unsigned bAmount) {
    const unsigned aAmount = 255 - bAmount;
    const auto channel = [=](unsigned shift) {
        return ((((a >> shift) & 0xFFu) * aAmount + ((b >> shift) & 0xFFu) * bAmount) / 255u) << shift;
    };
    return channel(16) | channel(8) | channel(0);
}

class ColorPickerWindow {
public:
    ColorPickerWindow(HWND owner, HINSTANCE instance, std::uint32_t initialColor,
                      UiLanguage language, const Profile& profile)
        : owner_(owner), instance_(instance), initialColor_(initialColor & 0xFFFFFFu),
          selectedColor_(initialColor_), hsv_(RgbToHsv(initialColor_)),
          strings_(&GetStrings(language)), profile_(&profile),
          palette_(profile.ResolvePalette()) {
        SyncFields();
    }

    std::optional<std::uint32_t> Show() {
        Register();
        const UINT dpi = owner_ ? GetDpiForWindow(owner_) : 96;
        const float scale = dpi / 96.0f;
        RECT rect{0, 0, static_cast<LONG>(760 * scale), static_cast<LONG>(520 * scale)};
        constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
        AdjustWindowRectExForDpi(&rect, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;

        const DialogPlacement placement = ResolveDialogPlacement(owner_);
        const int x = std::clamp(placement.anchor.left +
            ((placement.anchor.right - placement.anchor.left) - width) / 2,
            placement.workArea.left, std::max(placement.workArea.left, placement.workArea.right - width));
        const int y = std::clamp(placement.anchor.top +
            ((placement.anchor.bottom - placement.anchor.top) - height) / 2,
            placement.workArea.top, std::max(placement.workArea.top, placement.workArea.bottom - height));
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kColorPickerWindowClass,
            strings_->colorPickerTitle, style, x, y, width, height, owner_, nullptr,
            instance_, this);
        if (!window_) return std::nullopt;
        if (!surface_.Initialize(window_)) {
            DestroyWindow(window_);
            return std::nullopt;
        }

        ready_ = true;
        RefreshTheme();
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
        if (window_) DestroyWindow(window_);
        if (messageStatus == 0) PostQuitMessage(static_cast<int>(message.wParam));
        if (owner_ && IsWindow(owner_)) { EnableWindow(owner_, TRUE); SetActiveWindow(owner_); }
        return accepted_ ? std::optional<std::uint32_t>(selectedColor_) : std::nullopt;
    }

private:
    void Register() const {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kColorPickerWindowClass;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wc);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        try {
            auto* self = reinterpret_cast<ColorPickerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE) {
                self = static_cast<ColorPickerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
                self->window_ = window;
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
        } catch (...) {
            Security::FailFast(L"ColorPickerWindow::WindowProc");
        }
    }

    ColorPickerLayout Layout() const {
        const float s = surface_.Scale();
        const float w = surface_.Width();
        const float h = surface_.Height();
        const float p = 24 * s;
        ColorPickerLayout layout;
        layout.spectrumLabel = {p, 12 * s, 468 * s, 40 * s};
        layout.spectrum = {p, 44 * s, 416 * s, 344 * s};
        layout.hue = {432 * s, 44 * s, 468 * s, 344 * s};

        const float rightLeft = 492 * s;
        const float right = w - p;
        const float swatchGap = 12 * s;
        const float swatchWidth = (right - rightLeft - swatchGap) / 2;
        layout.currentLabel = {rightLeft, 12 * s, rightLeft + swatchWidth, 40 * s};
        layout.currentSwatch = {rightLeft, 44 * s, rightLeft + swatchWidth, 108 * s};
        layout.newLabel = {layout.currentSwatch.right + swatchGap, 12 * s, right, 40 * s};
        layout.newSwatch = {layout.currentSwatch.right + swatchGap, 44 * s, right, 108 * s};

        const float fieldGap = 8 * s;
        const float fieldWidth = (right - rightLeft - fieldGap * 2) / 3;
        for (std::size_t index = 0; index < 3; ++index) {
            const float left = rightLeft + index * (fieldWidth + fieldGap);
            layout.rgbLabels[index] = {left, 120 * s, left + fieldWidth, 146 * s};
            layout.rgbFields[index] = {left, 148 * s, left + fieldWidth, 190 * s};
        }
        layout.hexLabel = {rightLeft, 204 * s, right, 230 * s};
        layout.hexField = {rightLeft, 232 * s, right, 274 * s};
        layout.hint = {rightLeft, 282 * s, right, 310 * s};
        layout.error = {rightLeft, 318 * s, right, 370 * s};

        const float buttonWidth = 120 * s;
        const float buttonHeight = 40 * s;
        const float buttonGap = 8 * s;
        layout.cancel = {w - p - buttonWidth, h - p - buttonHeight, w - p, h - p};
        layout.apply = {layout.cancel.left - buttonGap - buttonWidth, layout.cancel.top,
                        layout.cancel.left - buttonGap, layout.cancel.bottom};
        return layout;
    }

    void RequestDraw() {
        // One posted frame absorbs high-frequency pointer movement while still
        // presenting each processed color state at the monitor refresh rate.
        if (ready_ && !renderQueued_) renderQueued_ = PostMessageW(window_, WM_COLOR_PICKER_RENDER, 0, 0) != FALSE;
    }

    void RefreshTheme() {
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

    static std::optional<std::size_t> FieldIndex(FocusItem focus) {
        if (focus >= FocusItem::Red && focus <= FocusItem::Hex)
            return static_cast<std::size_t>(static_cast<int>(focus) - static_cast<int>(FocusItem::Red));
        return std::nullopt;
    }

    static HoverItem FieldHover(std::size_t index) {
        return static_cast<HoverItem>(static_cast<int>(HoverItem::Red) + static_cast<int>(index));
    }

    static std::uint8_t Red(std::uint32_t color) { return static_cast<std::uint8_t>((color >> 16) & 0xFFu); }
    static std::uint8_t Green(std::uint32_t color) { return static_cast<std::uint8_t>((color >> 8) & 0xFFu); }
    static std::uint8_t Blue(std::uint32_t color) { return static_cast<std::uint8_t>(color & 0xFFu); }

    void SyncFields(std::optional<std::size_t> preserve = std::nullopt) {
        wchar_t text[16]{};
        const std::array<unsigned int, 3> channels{Red(selectedColor_), Green(selectedColor_), Blue(selectedColor_)};
        for (std::size_t index = 0; index < channels.size(); ++index) {
            if (preserve && *preserve == index) continue;
            swprintf_s(text, L"%u", channels[index]);
            fields_[index] = text;
        }
        if (!preserve || *preserve != 3) {
            swprintf_s(text, L"#%06X", selectedColor_ & 0xFFFFFFu);
            fields_[3] = text;
        }
    }

    static bool ParseDecimal(std::wstring_view text, std::uint8_t& value) {
        if (text.empty()) return false;
        unsigned int parsed = 0;
        for (wchar_t ch : text) {
            if (ch < L'0' || ch > L'9') return false;
            const unsigned int digit = static_cast<unsigned int>(ch - L'0');
            if (parsed > (255u - digit) / 10u) return false;
            parsed = parsed * 10u + digit;
        }
        value = static_cast<std::uint8_t>(parsed);
        return true;
    }

    static bool ParseHex(std::wstring_view text, std::uint32_t& value) {
        if (!text.empty() && text.front() == L'#') text.remove_prefix(1);
        if (text.size() != 6) return false;
        std::uint32_t parsed = 0;
        for (wchar_t ch : text) {
            unsigned int digit{};
            if (ch >= L'0' && ch <= L'9') digit = static_cast<unsigned int>(ch - L'0');
            else if (ch >= L'a' && ch <= L'f') digit = static_cast<unsigned int>(ch - L'a' + 10);
            else if (ch >= L'A' && ch <= L'F') digit = static_cast<unsigned int>(ch - L'A' + 10);
            else return false;
            parsed = (parsed << 4) | digit;
        }
        value = parsed;
        return true;
    }

    bool CommitField(std::size_t index, bool showError) {
        if (index < 3) {
            std::uint8_t channel{};
            if (!ParseDecimal(fields_[index], channel)) {
                if (showError) inlineError_ = strings_->colorPickerInvalidNumber;
                return false;
            }
            const unsigned int shift = static_cast<unsigned int>((2 - index) * 8);
            selectedColor_ = (selectedColor_ & ~(0xFFu << shift)) |
                             (static_cast<std::uint32_t>(channel) << shift);
        } else {
            std::uint32_t color{};
            if (!ParseHex(fields_[3], color)) {
                if (showError) inlineError_ = strings_->colorPickerInvalidHex;
                return false;
            }
            selectedColor_ = color;
        }
        hsv_ = RgbToHsv(selectedColor_);
        inlineError_.clear();
        SyncFields(index);
        return true;
    }

    void UpdateFromHsv() {
        selectedColor_ = HsvToRgb(hsv_);
        inlineError_.clear();
        selectAll_ = false;
        SyncFields();
        RequestDraw();
    }

    bool ChangeFocus(FocusItem next, bool selectField = true) {
        if (const auto field = FieldIndex(focus_); field && !CommitField(*field, true)) {
            RequestDraw();
            return false;
        }
        focus_ = next;
        selectAll_ = false;
        if (const auto field = FieldIndex(focus_)) {
            cursor_ = fields_[*field].size();
            selectAll_ = selectField;
        }
        caretVisible_ = true;
        RequestDraw();
        return true;
    }

    void UpdateSpectrum(float x, float y) {
        const auto rect = Layout().spectrum;
        hsv_.saturation = std::clamp((x - rect.left) / std::max(1.0f, rect.right - rect.left), 0.0f, 1.0f);
        hsv_.value = 1.0 - std::clamp((y - rect.top) / std::max(1.0f, rect.bottom - rect.top), 0.0f, 1.0f);
        UpdateFromHsv();
    }

    void UpdateHue(float y) {
        const auto rect = Layout().hue;
        const float position = std::clamp((y - rect.top) / std::max(1.0f, rect.bottom - rect.top), 0.0f, 1.0f);
        hsv_.hue = std::min(359.999, static_cast<double>(position) * 360.0);
        UpdateFromHsv();
    }

    void DrawButton(const D2D1_RECT_F& rect, const wchar_t* text, bool primary,
                    bool focused, bool hovered) {
        std::uint32_t background = primary ? palette_.selection : palette_.surface;
        if (hovered) background = Mix(background, primary ? palette_.selectionText : palette_.text,
                                      palette_.dark ? 22 : 14);
        const float s = surface_.Scale();
        surface_.FillRounded(rect, 4 * s, background);
        const std::uint32_t stroke = focused ? (primary ? palette_.selectionText : palette_.caret) : palette_.grid;
        surface_.StrokeRounded(rect, 4 * s, stroke, focused ? 2.0f * s : std::max(1.0f, s));
        surface_.Text(text, rect, primary ? palette_.selectionText : palette_.text, true, primary);
    }

    void DrawField(std::size_t index, const wchar_t* label, const D2D1_RECT_F& labelRect,
                   const D2D1_RECT_F& fieldRect) {
        const float s = surface_.Scale();
        const bool focused = FieldIndex(focus_) == index;
        const bool hovered = hover_ == FieldHover(index);
        surface_.CaptionText(label, labelRect, palette_.text);
        surface_.FillRounded(fieldRect, 4 * s, palette_.surface);
        surface_.StrokeRounded(fieldRect, 4 * s,
            focused ? palette_.caret : (hovered ? Mix(palette_.grid, palette_.text, 48) : palette_.grid),
            focused ? 2.0f * s : std::max(1.0f, s));
        const auto inner = D2D1::RectF(fieldRect.left + 10 * s, fieldRect.top + 1,
                                      fieldRect.right - 10 * s, fieldRect.bottom - 1);
        surface_.PushClip(inner);
        if (focused && selectAll_ && !fields_[index].empty()) {
            const float right = inner.left + surface_.TextPosition(fields_[index], fields_[index].size(), inner.right - inner.left);
            surface_.FillRounded({inner.left, inner.top + 8 * s, right, inner.bottom - 8 * s},
                                 2 * s, palette_.selection);
        }
        surface_.Text(fields_[index], inner, focused && selectAll_ ? palette_.selectionText : palette_.text);
        if (focused && caretVisible_ && !selectAll_) {
            const float x = inner.left + surface_.TextPosition(fields_[index], cursor_, inner.right - inner.left);
            surface_.Line({x, inner.top + 8 * s}, {x, inner.bottom - 8 * s}, palette_.caret, std::max(1.0f, s));
        }
        surface_.PopClip();
    }

    void Draw() {
        renderQueued_ = false;
        if (!surface_.Begin(palette_)) return;
        const auto layout = Layout();
        const float s = surface_.Scale();

        surface_.CaptionText(strings_->colorPickerSpectrum, layout.spectrumLabel, palette_.text);
        const std::uint32_t pureHue = HsvToRgb({hsv_.hue, 1.0, 1.0});
        surface_.Fill(layout.spectrum, pureHue);
        const std::array whiteOverlay{
            DialogGradientStop{0.0f, 0xFFFFFF, 1.0f},
            DialogGradientStop{1.0f, 0xFFFFFF, 0.0f}};
        const std::array blackOverlay{
            DialogGradientStop{0.0f, 0x000000, 0.0f},
            DialogGradientStop{1.0f, 0x000000, 1.0f}};
        surface_.FillLinearGradient(layout.spectrum, whiteOverlay, false);
        surface_.FillLinearGradient(layout.spectrum, blackOverlay, true);
        surface_.Stroke(layout.spectrum, focus_ == FocusItem::Spectrum ? palette_.caret : palette_.grid,
                        focus_ == FocusItem::Spectrum ? 2.0f * s : std::max(1.0f, s));

        const std::array hueStops{
            DialogGradientStop{0.0f, 0xFF0000}, DialogGradientStop{1.0f / 6.0f, 0xFFFF00},
            DialogGradientStop{2.0f / 6.0f, 0x00FF00}, DialogGradientStop{3.0f / 6.0f, 0x00FFFF},
            DialogGradientStop{4.0f / 6.0f, 0x0000FF}, DialogGradientStop{5.0f / 6.0f, 0xFF00FF},
            DialogGradientStop{1.0f, 0xFF0000}};
        surface_.FillLinearGradient(layout.hue, hueStops, true);
        surface_.Stroke(layout.hue, focus_ == FocusItem::Hue ? palette_.caret : palette_.grid,
                        focus_ == FocusItem::Hue ? 2.0f * s : std::max(1.0f, s));

        const float spectrumX = layout.spectrum.left + static_cast<float>(hsv_.saturation) *
            (layout.spectrum.right - layout.spectrum.left);
        const float spectrumY = layout.spectrum.top + static_cast<float>(1.0 - hsv_.value) *
            (layout.spectrum.bottom - layout.spectrum.top);
        const D2D1_ELLIPSE marker{{spectrumX, spectrumY}, 7 * s, 7 * s};
        surface_.Ellipse(marker, 0xFFFFFF, 3 * s);
        surface_.Ellipse(marker, 0x000000, std::max(1.0f, s));
        const float hueY = layout.hue.top + static_cast<float>(hsv_.hue / 360.0) *
            (layout.hue.bottom - layout.hue.top);
        const auto hueMarker = D2D1::RectF(layout.hue.left - 3 * s, hueY - 3 * s,
                                          layout.hue.right + 3 * s, hueY + 3 * s);
        surface_.Stroke(hueMarker, 0xFFFFFF, 3 * s);
        surface_.Stroke(hueMarker, 0x000000, std::max(1.0f, s));

        surface_.CaptionText(strings_->colorPickerCurrent, layout.currentLabel, palette_.text);
        surface_.CaptionText(strings_->colorPickerNew, layout.newLabel, palette_.text);
        surface_.FillRounded(layout.currentSwatch, 6 * s, initialColor_);
        surface_.StrokeRounded(layout.currentSwatch, 6 * s, palette_.grid, std::max(1.0f, s));
        surface_.FillRounded(layout.newSwatch, 6 * s, selectedColor_);
        surface_.StrokeRounded(layout.newSwatch, 6 * s, palette_.grid, std::max(1.0f, s));

        const std::array labels{strings_->colorPickerRed, strings_->colorPickerGreen, strings_->colorPickerBlue};
        for (std::size_t index = 0; index < 3; ++index)
            DrawField(index, labels[index], layout.rgbLabels[index], layout.rgbFields[index]);
        DrawField(3, strings_->colorPickerHex, layout.hexLabel, layout.hexField);
        surface_.Text(strings_->colorPickerHint, layout.hint, palette_.mutedText);

        if (!inlineError_.empty()) {
            const std::uint32_t errorColor = palette_.highContrast ? palette_.text : (palette_.dark ? 0xFF8A80 : 0xB3261E);
            const std::uint32_t errorSurface = palette_.highContrast ? palette_.surface : Mix(palette_.background, errorColor, 18);
            surface_.FillRounded(layout.error, 4 * s, errorSurface);
            surface_.StrokeRounded(layout.error, 4 * s, errorColor, std::max(1.0f, s));
            surface_.Text(inlineError_, {layout.error.left + 10 * s, layout.error.top,
                          layout.error.right - 10 * s, layout.error.bottom}, errorColor);
        }

        DrawButton(layout.apply, strings_->colorPickerApply, true,
                   focus_ == FocusItem::Apply, hover_ == HoverItem::Apply);
        DrawButton(layout.cancel, strings_->cancelButton, false,
                   focus_ == FocusItem::Cancel, hover_ == HoverItem::Cancel);
        if (!surface_.End()) RequestDraw();
    }

    HoverItem Hit(float x, float y) const {
        const auto layout = Layout();
        if (Contains(layout.spectrum, x, y)) return HoverItem::Spectrum;
        if (Contains(layout.hue, x, y)) return HoverItem::Hue;
        for (std::size_t index = 0; index < 3; ++index)
            if (Contains(layout.rgbFields[index], x, y)) return FieldHover(index);
        if (Contains(layout.hexField, x, y)) return HoverItem::Hex;
        if (Contains(layout.apply, x, y)) return HoverItem::Apply;
        if (Contains(layout.cancel, x, y)) return HoverItem::Cancel;
        return HoverItem::None;
    }

    void BeginPointerAction(HoverItem item, float x, float y) {
        if (item == HoverItem::Spectrum) {
            if (!ChangeFocus(FocusItem::Spectrum, false)) return;
            drag_ = DragTarget::Spectrum;
            SetCapture(window_);
            UpdateSpectrum(x, y);
        } else if (item == HoverItem::Hue) {
            if (!ChangeFocus(FocusItem::Hue, false)) return;
            drag_ = DragTarget::Hue;
            SetCapture(window_);
            UpdateHue(y);
        } else if (item >= HoverItem::Red && item <= HoverItem::Hex) {
            ChangeFocus(static_cast<FocusItem>(static_cast<int>(FocusItem::Red) +
                static_cast<int>(item) - static_cast<int>(HoverItem::Red)));
        } else if (item == HoverItem::Apply) {
            Accept();
        } else if (item == HoverItem::Cancel) {
            Close();
        }
    }

    void Accept() {
        if (const auto field = FieldIndex(focus_); field && !CommitField(*field, true)) {
            RequestDraw();
            return;
        }
        accepted_ = true;
        Close();
    }

    void Close() const {
        if (window_) DestroyWindow(window_);
    }

    void CycleFocus(bool reverse) {
        const int count = 8;
        const int value = (static_cast<int>(focus_) + (reverse ? count - 1 : 1)) % count;
        ChangeFocus(static_cast<FocusItem>(value));
    }

    void AdjustFocused(int direction, bool coarse) {
        const double step = coarse ? 0.05 : 1.0 / 255.0;
        if (focus_ == FocusItem::Spectrum) {
            hsv_.saturation = std::clamp(hsv_.saturation + direction * step, 0.0, 1.0);
            UpdateFromHsv();
        } else if (focus_ == FocusItem::Hue) {
            hsv_.hue += direction * (coarse ? 10.0 : 1.0);
            while (hsv_.hue < 0.0) hsv_.hue += 360.0;
            while (hsv_.hue >= 360.0) hsv_.hue -= 360.0;
            UpdateFromHsv();
        }
    }

    void AdjustValue(int direction, bool coarse) {
        const double step = coarse ? 0.05 : 1.0 / 255.0;
        hsv_.value = std::clamp(hsv_.value + direction * step, 0.0, 1.0);
        UpdateFromHsv();
    }

    void IncrementField(std::size_t index, int direction, bool coarse) {
        if (index >= 3) return;
        std::uint8_t current = index == 0 ? Red(selectedColor_) :
            (index == 1 ? Green(selectedColor_) : Blue(selectedColor_));
        const int adjusted = std::clamp(static_cast<int>(current) + direction * (coarse ? 10 : 1), 0, 255);
        fields_[index] = std::to_wstring(adjusted);
        cursor_ = fields_[index].size();
        selectAll_ = false;
        CommitField(index, false);
        RequestDraw();
    }

    void InsertFieldText(std::wstring_view text) {
        const auto field = FieldIndex(focus_);
        if (!field) return;
        inlineError_.clear();
        auto& value = fields_[*field];
        if (selectAll_) {
            value.clear();
            cursor_ = 0;
            selectAll_ = false;
        }
        const std::size_t maximum = *field == 3 ? 7 : 3;
        const std::size_t available = maximum - std::min(maximum, value.size());
        text = text.substr(0, available);
        value.insert(cursor_, text);
        cursor_ += text.size();
        CommitField(*field, false);
        caretVisible_ = true;
        RequestDraw();
    }

    void CopySelection(bool cut) {
        const auto field = FieldIndex(focus_);
        if (!field || !selectAll_ || fields_[*field].empty() || !OpenClipboard(window_)) return;
        EmptyClipboard();
        const SIZE_T bytes = (fields_[*field].size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            void* data = GlobalLock(memory);
            if (data) {
                memcpy(data, fields_[*field].c_str(), bytes);
                GlobalUnlock(memory);
            }
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        }
        CloseClipboard();
        if (cut) {
            fields_[*field].clear();
            cursor_ = 0;
            selectAll_ = false;
            RequestDraw();
        }
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
                if (end != text + capacity) InsertFieldText(std::wstring_view(text, end));
                GlobalUnlock(value);
            }
        }
        CloseClipboard();
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window_, &paint);
            EndPaint(window_, &paint);
            RequestDraw();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (ready_) {
                surface_.Resize(LOWORD(lParam), HIWORD(lParam));
                RequestDraw();
            }
            return 0;
        case WM_DPICHANGED: {
            surface_.DpiChanged(HIWORD(wParam));
            const RECT* rect = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window_, nullptr, rect->left, rect->top,
                         rect->right - rect->left, rect->bottom - rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            RequestDraw();
            return 0;
        }
        case WM_COLOR_PICKER_RENDER:
            Draw();
            return 0;
        case WM_SETTINGCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
            RefreshTheme();
            return 0;
        case WM_TIMER:
            if (wParam == kCaretTimer && FieldIndex(focus_)) {
                caretVisible_ = !caretVisible_;
                RequestDraw();
            }
            return 0;
        case WM_MOUSEMOVE: {
            const float x = static_cast<float>(GET_X_LPARAM(lParam));
            const float y = static_cast<float>(GET_Y_LPARAM(lParam));
            if (drag_ == DragTarget::Spectrum) UpdateSpectrum(x, y);
            else if (drag_ == DragTarget::Hue) UpdateHue(y);
            else {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
                TrackMouseEvent(&track);
                const HoverItem item = Hit(x, y);
                if (item != hover_) {
                    hover_ = item;
                    RequestDraw();
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = HoverItem::None;
            RequestDraw();
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(window_);
            BeginPointerAction(Hit(static_cast<float>(GET_X_LPARAM(lParam)),
                                   static_cast<float>(GET_Y_LPARAM(lParam))),
                               static_cast<float>(GET_X_LPARAM(lParam)),
                               static_cast<float>(GET_Y_LPARAM(lParam)));
            return 0;
        case WM_LBUTTONUP:
            if (GetCapture() == window_) ReleaseCapture();
            drag_ = DragTarget::None;
            return 0;
        case WM_KEYDOWN: {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (wParam == VK_ESCAPE) { Close(); return 0; }
            if (wParam == VK_RETURN) {
                if (focus_ == FocusItem::Cancel) Close();
                else Accept();
                return 0;
            }
            if (wParam == VK_TAB) { CycleFocus(shift); return 0; }
            if (focus_ == FocusItem::Spectrum) {
                if (wParam == VK_LEFT) { AdjustFocused(-1, shift); return 0; }
                if (wParam == VK_RIGHT) { AdjustFocused(1, shift); return 0; }
                if (wParam == VK_UP) { AdjustValue(1, shift); return 0; }
                if (wParam == VK_DOWN) { AdjustValue(-1, shift); return 0; }
            } else if (focus_ == FocusItem::Hue) {
                if (wParam == VK_LEFT || wParam == VK_UP) { AdjustFocused(-1, shift); return 0; }
                if (wParam == VK_RIGHT || wParam == VK_DOWN) { AdjustFocused(1, shift); return 0; }
            } else if (const auto field = FieldIndex(focus_)) {
                auto& value = fields_[*field];
                if (ctrl && wParam == 'A') { selectAll_ = true; cursor_ = value.size(); RequestDraw(); return 0; }
                if (ctrl && wParam == 'C') { CopySelection(false); return 0; }
                if (ctrl && wParam == 'X') { CopySelection(true); return 0; }
                if (ctrl && wParam == 'V') { Paste(); return 0; }
                if (*field < 3 && (wParam == VK_UP || wParam == VK_DOWN)) {
                    IncrementField(*field, wParam == VK_UP ? 1 : -1, shift);
                    return 0;
                }
                if (wParam == VK_LEFT) { selectAll_ = false; if (cursor_) --cursor_; RequestDraw(); return 0; }
                if (wParam == VK_RIGHT) { selectAll_ = false; cursor_ = std::min(value.size(), cursor_ + 1); RequestDraw(); return 0; }
                if (wParam == VK_HOME) { selectAll_ = false; cursor_ = 0; RequestDraw(); return 0; }
                if (wParam == VK_END) { selectAll_ = false; cursor_ = value.size(); RequestDraw(); return 0; }
                if (wParam == VK_DELETE) {
                    inlineError_.clear();
                    if (selectAll_) { value.clear(); cursor_ = 0; selectAll_ = false; }
                    else if (cursor_ < value.size()) value.erase(cursor_, 1);
                    CommitField(*field, false);
                    RequestDraw();
                    return 0;
                }
            } else if (wParam == VK_SPACE) {
                if (focus_ == FocusItem::Apply) Accept();
                else if (focus_ == FocusItem::Cancel) Close();
                return 0;
            }
            break;
        }
        case WM_CHAR:
            if (const auto field = FieldIndex(focus_)) {
                if (wParam == VK_BACK) {
                    auto& value = fields_[*field];
                    inlineError_.clear();
                    if (selectAll_) { value.clear(); cursor_ = 0; selectAll_ = false; }
                    else if (cursor_) value.erase(--cursor_, 1);
                    CommitField(*field, false);
                    RequestDraw();
                } else if (wParam >= 0x20) {
                    const wchar_t character = static_cast<wchar_t>(wParam);
                    const bool decimal = character >= L'0' && character <= L'9';
                    const bool hexadecimal = decimal || (character >= L'a' && character <= L'f') ||
                        (character >= L'A' && character <= L'F') || character == L'#';
                    if ((*field < 3 && decimal) || (*field == 3 && hexadecimal))
                        InsertFieldText(std::wstring_view(&character, 1));
                }
            }
            return 0;
        case WM_CLOSE:
            Close();
            return 0;
        case WM_DESTROY:
            if (GetCapture() == window_) ReleaseCapture();
            KillTimer(window_, kCaretTimer);
            running_ = false;
            window_ = nullptr;
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    HWND owner_{};
    HINSTANCE instance_{};
    HWND window_{};
    std::uint32_t initialColor_{};
    std::uint32_t selectedColor_{};
    HsvColor hsv_{};
    const UiStrings* strings_{};
    const Profile* profile_{};
    Palette palette_{};
    D2DDialogSurface surface_{};
    std::array<std::wstring, kFieldCount> fields_{};
    std::wstring inlineError_;
    std::size_t cursor_{};
    FocusItem focus_{FocusItem::Spectrum};
    HoverItem hover_{HoverItem::None};
    DragTarget drag_{DragTarget::None};
    bool running_{true};
    bool ready_{};
    bool renderQueued_{};
    bool caretVisible_{true};
    bool selectAll_{};
    bool applyingTheme_{};
    bool accepted_{};
};
}

std::optional<std::uint32_t> ShowColorPickerDialog(HWND owner, HINSTANCE instance,
    std::uint32_t initialColor, UiLanguage language, const Profile& profile) {
    ColorPickerWindow window(owner, instance, initialColor, language, profile);
    return window.Show();
}
