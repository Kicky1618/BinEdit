// Implements BinEdit's fully themed bit-operation tool as a custom D3D11/D2D1
// modal window. No native child controls are used, which keeps light, dark, and
// high-contrast behavior identical to the search and About dialogs.

#include "BitToolDialog.h"

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

namespace {
constexpr wchar_t kBitToolWindowClass[] = L"BinEdit.BitToolDialog";
// Posted rendering coalesces mouse, caret, keyboard, DPI, and theme invalidation.
constexpr UINT WM_BIT_TOOL_RENDER = WM_APP + 44;
constexpr UINT_PTR kCaretTimer = 1;
constexpr std::size_t kOperationCount = 12;
constexpr int kOperationColumns = 4;

enum class FocusItem { Operations, Input, Operand, Close };

// Operation hover values are contiguous so hit testing can map directly to the
// strongly typed BitOperation enum whose declaration uses the same order.
enum class HoverItem {
    None,
    Operation0,
    Operation1,
    Operation2,
    Operation3,
    Operation4,
    Operation5,
    Operation6,
    Operation7,
    Operation8,
    Operation9,
    Operation10,
    Operation11,
    Input,
    Operand,
    Close
};

struct BitToolLayout {
    D2D1_RECT_F operationLabel{};
    std::array<D2D1_RECT_F, kOperationCount> operations{};
    D2D1_RECT_F inputLabel{}, input{}, inputHint{};
    D2D1_RECT_F operandLabel{}, operand{}, operandHint{};
    D2D1_RECT_F previewLabel{}, preview{}, wrapHint{}, error{}, close{};
};

bool Contains(D2D1_RECT_F rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

// Blends two opaque RGB values for deterministic hover surfaces without adding
// alpha-composition state to the shared dialog renderer.
std::uint32_t Mix(std::uint32_t a, std::uint32_t b, unsigned bAmount) {
    const unsigned aAmount = 255 - bAmount;
    const auto channel = [=](unsigned shift) {
        return ((((a >> shift) & 0xFF) * aAmount + ((b >> shift) & 0xFF) * bAmount) / 255) << shift;
    };
    return channel(16) | channel(8) | channel(0);
}

class BitToolWindow {
public:
    BitToolWindow(HWND owner, HINSTANCE instance, const BitToolDialogResult& initial,
                  UiLanguage language, const Profile& profile)
        : owner_(owner), instance_(instance), result_(initial), strings_(&GetStrings(language)),
          profile_(&profile), palette_(profile.ResolvePalette()) {
        wchar_t input[8]{};
        swprintf_s(input, L"0x%02X", static_cast<unsigned int>(result_.input));
        inputText_ = input;
        inputCursor_ = inputText_.size();
        wchar_t operand[8]{};
        swprintf_s(operand, L"0x%02X", static_cast<unsigned int>(result_.operand));
        operandText_ = operand;
        operandCursor_ = operandText_.size();
    }

    BitToolDialogResult Show() {
        Register();
        const UINT dpi = owner_ ? GetDpiForWindow(owner_) : 96;
        const float scale = dpi / 96.0f;
        RECT rect{0, 0, static_cast<LONG>(720 * scale), static_cast<LONG>(560 * scale)};
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
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kBitToolWindowClass, strings_->bitToolTitle,
            style, x, y, width, height, owner_, nullptr, instance_, this);
        if (!window_) return result_;
        if (!surface_.Initialize(window_)) {
            DestroyWindow(window_);
            return result_;
        }

        ready_ = true;
        RefreshTheme();
        // Owner disabling plus a nested UI-thread loop provides MessageBox-style
        // modality while preserving GPU presentation and IME-aware input dispatch.
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
        // A process-wide quit observed inside the nested loop must reach the main
        // loop after the modal owner has been restored.
        if (window_) DestroyWindow(window_);
        if (messageStatus == 0) PostQuitMessage(static_cast<int>(message.wParam));
        if (owner_ && IsWindow(owner_)) { EnableWindow(owner_, TRUE); SetActiveWindow(owner_); }
        return result_;
    }

private:
    void Register() const {
        // RegisterClassExW is process-idempotent; an existing class is reusable.
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kBitToolWindowClass;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wc);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        try {
            // The stack-owned controller remains alive for the complete modal loop.
            auto* self = reinterpret_cast<BitToolWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE) {
                self = static_cast<BitToolWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
                self->window_ = window;
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
        } catch (...) {
            Security::FailFast(L"BitToolWindow::WindowProc");
        }
    }

    BitToolLayout Layout() const {
        const float s = surface_.Scale();
        const float w = surface_.Width();
        const float h = surface_.Height();
        const float p = 24 * s;
        const float gap = 8 * s;
        BitToolLayout layout;
        layout.operationLabel = {p, 16 * s, w - p, 40 * s};
        const float optionWidth = (w - p * 2 - gap * (kOperationColumns - 1)) / kOperationColumns;
        for (std::size_t i = 0; i < layout.operations.size(); ++i) {
            const int column = static_cast<int>(i) % kOperationColumns;
            const int row = static_cast<int>(i) / kOperationColumns;
            const float left = p + column * (optionWidth + gap);
            const float top = (44 + row * 48) * s;
            layout.operations[i] = {left, top, left + optionWidth, top + 40 * s};
        }
        layout.inputLabel = {p, 194 * s, 244 * s, 220 * s};
        layout.input = {p, 222 * s, 244 * s, 264 * s};
        layout.inputHint = {260 * s, 222 * s, w - p, 264 * s};
        layout.operandLabel = {p, 276 * s, 244 * s, 302 * s};
        layout.operand = {p, 304 * s, 244 * s, 346 * s};
        layout.operandHint = {260 * s, 304 * s, w - p, 346 * s};
        layout.previewLabel = {p, 358 * s, w - p, 384 * s};
        layout.preview = {p, 386 * s, w - p, 430 * s};
        layout.wrapHint = {p, 436 * s, w - p, 460 * s};
        layout.error = {p, 464 * s, w - p, 492 * s};
        const float buttonWidth = 120 * s;
        const float buttonHeight = 40 * s;
        layout.close = {w - p - buttonWidth, h - p - buttonHeight, w - p, h - p};
        return layout;
    }

    std::array<const wchar_t*, kOperationCount> OperationLabels() const {
        return {strings_->bitOpAnd, strings_->bitOpOr, strings_->bitOpXor, strings_->bitOpNot,
                strings_->bitOpShiftLeft, strings_->bitOpShiftRight,
                strings_->bitOpRotateLeft, strings_->bitOpRotateRight,
                strings_->bitOpAdd, strings_->bitOpSubtract,
                strings_->bitOpMultiply, strings_->bitOpDivide};
    }

    std::size_t OperationIndex() const {
        const auto index = static_cast<std::size_t>(result_.operation);
        return std::min(index, kOperationCount - 1);
    }

    void RequestDraw() {
        // R2PR-style coalescing guarantees at most one pending frame regardless of
        // how many input or theme messages describe the same visual state.
        if (ready_ && !renderQueued_) renderQueued_ = PostMessageW(window_, WM_BIT_TOOL_RENDER, 0, 0) != FALSE;
    }

    void RefreshTheme() {
        if (applyingTheme_) return;
        applyingTheme_ = true;
        // Re-resolve on every system notification so high-contrast and the user's
        // Windows accent color update while the dialog remains open.
        palette_ = profile_->ResolvePalette();
        BOOL dark = palette_.dark && !palette_.highContrast;
        constexpr DWORD immersiveDarkMode = 20;
        DwmSetWindowAttribute(window_, immersiveDarkMode, &dark, sizeof(dark));
        SetWindowTheme(window_, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
        applyingTheme_ = false;
        RequestDraw();
    }

    void DrawButton(const D2D1_RECT_F& rect, const wchar_t* text, bool primary,
                    bool focused, bool hovered, bool enabled = true) {
        std::uint32_t background = primary && enabled ? palette_.selection : palette_.surface;
        if (hovered) background = Mix(background, primary ? palette_.selectionText : palette_.text,
                                      palette_.dark ? 22 : 14);
        const float s = surface_.Scale();
        surface_.FillRounded(rect, 4 * s, background);
        const std::uint32_t stroke = focused ? (primary && enabled ? palette_.selectionText : palette_.caret) : palette_.grid;
        surface_.StrokeRounded(rect, 4 * s, stroke, focused ? 2.0f * s : std::max(1.0f, s));
        // The shared centered-bold DirectWrite format fixes vertical and horizontal
        // placement for the primary action in every DPI and language configuration.
        const std::uint32_t foreground = enabled ?
            (primary ? palette_.selectionText : palette_.text) : palette_.mutedText;
        surface_.Text(text, rect, foreground, true, primary && enabled);
    }

    void DrawEditField(const D2D1_RECT_F& rect, std::wstring_view text,
                       FocusItem item, HoverItem hoverItem, bool enabled,
                       std::size_t cursor, bool selectAll) {
        const float s = surface_.Scale();
        const bool focused = enabled && focus_ == item;
        surface_.FillRounded(rect, 4 * s, palette_.surface);
        const std::uint32_t stroke = focused ? palette_.caret :
            (hover_ == hoverItem && enabled ? Mix(palette_.grid, palette_.text, 48) : palette_.grid);
        surface_.StrokeRounded(rect, 4 * s, stroke,
                               focused ? 2.0f * s : std::max(1.0f, s));
        const auto inner = D2D1::RectF(rect.left + 11 * s, rect.top + 1,
                                      rect.right - 11 * s, rect.bottom - 1);
        surface_.PushClip(inner);
        if (selectAll && enabled && !text.empty()) {
            const float selectedRight = inner.left + surface_.TextPosition(text, text.size(), inner.right - inner.left);
            surface_.FillRounded({inner.left, inner.top + 8 * s, selectedRight, inner.bottom - 8 * s},
                                 2 * s, palette_.selection);
        }
        surface_.Text(text, inner, selectAll && enabled ? palette_.selectionText :
                      (enabled ? palette_.text : palette_.mutedText));
        if (focused && caretVisible_ && !selectAll) {
            const float x = inner.left + surface_.TextPosition(text, cursor, inner.right - inner.left);
            surface_.Line({x, inner.top + 8 * s}, {x, inner.bottom - 8 * s},
                          palette_.caret, std::max(1.0f, s));
        }
        surface_.PopClip();
    }

    static bool ParseByte(std::wstring_view text, std::uint8_t& value) {
        if (text.empty()) return false;
        std::size_t position = 0;
        unsigned int base = 10;
        if (text.size() >= 2 && text[0] == L'0' &&
            (text[1] == L'x' || text[1] == L'X')) {
            base = 16;
            position = 2;
        }
        if (position == text.size()) return false;

        unsigned int parsed = 0;
        for (; position < text.size(); ++position) {
            const wchar_t ch = text[position];
            unsigned int digit = 0;
            if (ch >= L'0' && ch <= L'9') digit = static_cast<unsigned int>(ch - L'0');
            else if (ch >= L'a' && ch <= L'f') digit = static_cast<unsigned int>(ch - L'a' + 10);
            else if (ch >= L'A' && ch <= L'F') digit = static_cast<unsigned int>(ch - L'A' + 10);
            else return false;
            if (digit >= base || parsed > (255u - digit) / base) return false;
            parsed = parsed * base + digit;
        }
        value = static_cast<std::uint8_t>(parsed);
        return true;
    }

    bool ParseOperand(std::uint8_t& value) const {
        if (!BitOperationRequiresOperand(result_.operation)) {
            value = result_.operand;
            return true;
        }
        return ParseByte(operandText_, value);
    }

    const wchar_t* ValidationError(std::uint8_t& input, std::uint8_t& operand) const {
        if (!ParseByte(inputText_, input)) return strings_->bitToolInvalidInput;
        if (!ParseOperand(operand)) return strings_->bitToolInvalidOperand;
        if (BitOperationUsesBitCount(result_.operation) && operand > 7) return strings_->bitToolBitCountRange;
        if (result_.operation == BitOperation::Divide && operand == 0) return strings_->bitToolDivideByZero;
        return nullptr;
    }

    void Draw() {
        renderQueued_ = false;
        if (!surface_.Begin(palette_)) return;
        const auto layout = Layout();
        const auto labels = OperationLabels();
        const float s = surface_.Scale();

        surface_.CaptionText(strings_->bitToolOperation, layout.operationLabel, palette_.text);

        for (std::size_t i = 0; i < layout.operations.size(); ++i) {
            const bool selected = i == OperationIndex();
            const bool focused = selected && focus_ == FocusItem::Operations;
            const bool hovered = hover_ == static_cast<HoverItem>(static_cast<int>(HoverItem::Operation0) + static_cast<int>(i));
            std::uint32_t background = selected ? palette_.selection : palette_.surface;
            if (hovered) background = Mix(background, selected ? palette_.selectionText : palette_.text,
                                          palette_.dark ? 22 : 14);
            surface_.FillRounded(layout.operations[i], 4 * s, background);
            const std::uint32_t stroke = focused ? (selected ? palette_.selectionText : palette_.caret) : palette_.grid;
            surface_.StrokeRounded(layout.operations[i], 4 * s, stroke,
                                   focused ? 2.0f * s : std::max(1.0f, s));
            surface_.Text(labels[i], layout.operations[i], selected ? palette_.selectionText : palette_.text,
                          true, selected);
        }

        surface_.CaptionText(strings_->bitToolInput, layout.inputLabel, palette_.text);
        DrawEditField(layout.input, inputText_, FocusItem::Input, HoverItem::Input,
                      true, inputCursor_, inputSelectAll_);
        surface_.Text(strings_->bitToolInputHint, layout.inputHint, palette_.mutedText);

        const bool operandEnabled = BitOperationRequiresOperand(result_.operation);
        surface_.CaptionText(strings_->bitToolOperand, layout.operandLabel,
                             operandEnabled ? palette_.text : palette_.mutedText);
        DrawEditField(layout.operand, operandText_, FocusItem::Operand, HoverItem::Operand,
                      operandEnabled, operandCursor_, operandSelectAll_);
        surface_.Text(strings_->bitToolOperandHint, layout.operandHint, operandEnabled ? palette_.mutedText : palette_.grid);

        surface_.CaptionText(strings_->bitToolPreview, layout.previewLabel, palette_.text);
        surface_.FillRounded(layout.preview, 6 * s, palette_.surface);
        surface_.StrokeRounded(layout.preview, 6 * s, palette_.grid, std::max(1.0f, s));
        std::uint8_t input{};
        std::uint8_t operand{};
        std::uint8_t transformed{};
        const wchar_t* validation = ValidationError(input, operand);
        wchar_t preview[64]{};
        if (!validation && ApplyBitOperation(input, result_.operation, operand, transformed))
            swprintf_s(preview, L"%02X   \u2192   %02X", static_cast<unsigned int>(input), static_cast<unsigned int>(transformed));
        else
            swprintf_s(preview, L"--   \u2192   --");
        surface_.TitleText(preview, layout.preview, palette_.text, true);
        surface_.Text(strings_->bitToolWrapHint, layout.wrapHint, palette_.mutedText);

        if (validation) {
            const std::uint32_t errorColor = palette_.highContrast ? palette_.text : (palette_.dark ? 0xFF8A80 : 0xB3261E);
            const std::uint32_t errorSurface = palette_.highContrast ? palette_.surface : Mix(palette_.background, errorColor, 18);
            surface_.FillRounded(layout.error, 4 * s, errorSurface);
            surface_.StrokeRounded(layout.error, 4 * s, errorColor, std::max(1.0f, s));
            surface_.Text(validation, {layout.error.left + 12 * s, layout.error.top,
                          layout.error.right - 12 * s, layout.error.bottom}, errorColor);
        }

        DrawButton(layout.close, strings_->bitToolClose, true,
                   focus_ == FocusItem::Close, hover_ == HoverItem::Close);
        if (!surface_.End()) RequestDraw();
    }

    HoverItem Hit(float x, float y) const {
        const auto layout = Layout();
        for (std::size_t i = 0; i < layout.operations.size(); ++i) {
            if (Contains(layout.operations[i], x, y))
                return static_cast<HoverItem>(static_cast<int>(HoverItem::Operation0) + static_cast<int>(i));
        }
        if (Contains(layout.input, x, y)) return HoverItem::Input;
        if (Contains(layout.operand, x, y) && BitOperationRequiresOperand(result_.operation)) return HoverItem::Operand;
        if (Contains(layout.close, x, y)) return HoverItem::Close;
        return HoverItem::None;
    }

    void SelectOperation(std::size_t index) {
        result_.operation = static_cast<BitOperation>(std::min(index, kOperationCount - 1));
        focus_ = FocusItem::Operations;
        inputSelectAll_ = false;
        operandSelectAll_ = false;
        caretVisible_ = true;
        RequestDraw();
    }

    void Activate(HoverItem item) {
        if (item >= HoverItem::Operation0 && item <= HoverItem::Operation11) {
            SelectOperation(static_cast<std::size_t>(static_cast<int>(item) - static_cast<int>(HoverItem::Operation0)));
        } else if (item == HoverItem::Input) {
            focus_ = FocusItem::Input;
            inputCursor_ = inputText_.size();
            inputSelectAll_ = false;
            caretVisible_ = true;
            RequestDraw();
        } else if (item == HoverItem::Operand) {
            focus_ = FocusItem::Operand;
            operandCursor_ = operandText_.size();
            operandSelectAll_ = false;
            caretVisible_ = true;
            RequestDraw();
        } else if (item == HoverItem::Close) {
            Close();
        }
    }

    void StoreValidSettings() {
        std::uint8_t value{};
        if (ParseByte(inputText_, value)) result_.input = value;
        if (ParseOperand(value)) result_.operand = value;
    }

    void Close() {
        // Dismissing the tool remembers every syntactically valid field so the
        // next calculation resumes from the same dialog-owned values.
        StoreValidSettings();
        if (window_) DestroyWindow(window_);
    }

    bool HasEditableField() const noexcept {
        return focus_ == FocusItem::Input ||
            (focus_ == FocusItem::Operand && BitOperationRequiresOperand(result_.operation));
    }

    std::wstring& FocusedText() {
        return focus_ == FocusItem::Input ? inputText_ : operandText_;
    }

    std::size_t& FocusedCursor() {
        return focus_ == FocusItem::Input ? inputCursor_ : operandCursor_;
    }

    bool& FocusedSelectAll() {
        return focus_ == FocusItem::Input ? inputSelectAll_ : operandSelectAll_;
    }

    void InsertText(std::wstring_view text) {
        if (!HasEditableField()) return;
        std::wstring& field = FocusedText();
        std::size_t& cursor = FocusedCursor();
        bool& selectAll = FocusedSelectAll();
        if (selectAll) {
            field.clear();
            cursor = 0;
            selectAll = false;
        }
        // The bounded field prevents pathological clipboard input from increasing
        // per-frame shaping cost. Normal decimal/hex values need at most four chars.
        const std::size_t available = 16 - std::min<std::size_t>(16, field.size());
        text = text.substr(0, available);
        field.insert(cursor, text);
        cursor += text.size();
        caretVisible_ = true;
        RequestDraw();
    }

    void CopySelection(bool cut) {
        // Ctrl+C/X operate on the explicit whole-field selection created by Ctrl+A.
        if (!HasEditableField()) return;
        std::wstring& field = FocusedText();
        std::size_t& cursor = FocusedCursor();
        bool& selectAll = FocusedSelectAll();
        if (!selectAll || field.empty() || !OpenClipboard(window_)) return;
        EmptyClipboard();
        const SIZE_T bytes = (field.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            void* data = GlobalLock(memory);
            if (data) {
                memcpy(data, field.c_str(), bytes);
                GlobalUnlock(memory);
                if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
            } else {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
        if (cut) {
            field.clear();
            cursor = 0;
            selectAll = false;
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
                if (end != text + capacity) InsertText(std::wstring_view(text, end));
                GlobalUnlock(value);
            }
        }
        CloseClipboard();
    }

    void CycleFocus(bool reverse) {
        // Unary NOT omits its disabled operand field from keyboard traversal.
        int value = static_cast<int>(focus_);
        do {
            value = (value + (reverse ? 3 : 1)) % 4;
        } while (value == static_cast<int>(FocusItem::Operand) &&
                 !BitOperationRequiresOperand(result_.operation));
        focus_ = static_cast<FocusItem>(value);
        inputSelectAll_ = false;
        operandSelectAll_ = false;
        caretVisible_ = true;
        RequestDraw();
    }

    void MoveOperation(int delta) {
        const int count = static_cast<int>(kOperationCount);
        int index = static_cast<int>(OperationIndex());
        index = (index + delta) % count;
        if (index < 0) index += count;
        SelectOperation(static_cast<std::size_t>(index));
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
        case WM_BIT_TOOL_RENDER:
            Draw();
            return 0;
        case WM_SETTINGCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
            RefreshTheme();
            return 0;
        case WM_TIMER:
            if (wParam == kCaretTimer && HasEditableField()) {
                caretVisible_ = !caretVisible_;
                RequestDraw();
            }
            return 0;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
            TrackMouseEvent(&track);
            const auto item = Hit(static_cast<float>(GET_X_LPARAM(lParam)),
                                  static_cast<float>(GET_Y_LPARAM(lParam)));
            if (item != hover_) {
                hover_ = item;
                RequestDraw();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = HoverItem::None;
            RequestDraw();
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(window_);
            Activate(Hit(static_cast<float>(GET_X_LPARAM(lParam)),
                         static_cast<float>(GET_Y_LPARAM(lParam))));
            return 0;
        case WM_KEYDOWN: {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (wParam == VK_ESCAPE) {
                Close();
                return 0;
            }
            if (wParam == VK_RETURN) {
                Close();
                return 0;
            }
            if (wParam == VK_TAB) {
                CycleFocus(shift);
                return 0;
            }
            if (focus_ == FocusItem::Operations) {
                if (wParam == VK_LEFT) { MoveOperation(-1); return 0; }
                if (wParam == VK_RIGHT) { MoveOperation(1); return 0; }
                if (wParam == VK_UP) { MoveOperation(-kOperationColumns); return 0; }
                if (wParam == VK_DOWN) { MoveOperation(kOperationColumns); return 0; }
            } else if (HasEditableField()) {
                std::wstring& field = FocusedText();
                std::size_t& cursor = FocusedCursor();
                bool& selectAll = FocusedSelectAll();
                if (ctrl && wParam == 'A') {
                    selectAll = true;
                    cursor = field.size();
                    RequestDraw();
                    return 0;
                }
                if (ctrl && wParam == 'C') { CopySelection(false); return 0; }
                if (ctrl && wParam == 'X') { CopySelection(true); return 0; }
                if (ctrl && wParam == 'V') { Paste(); return 0; }
                if (wParam == VK_LEFT) {
                    selectAll = false;
                    if (cursor) --cursor;
                    RequestDraw();
                    return 0;
                }
                if (wParam == VK_RIGHT) {
                    selectAll = false;
                    cursor = std::min(field.size(), cursor + 1);
                    RequestDraw();
                    return 0;
                }
                if (wParam == VK_HOME) {
                    selectAll = false;
                    cursor = 0;
                    RequestDraw();
                    return 0;
                }
                if (wParam == VK_END) {
                    selectAll = false;
                    cursor = field.size();
                    RequestDraw();
                    return 0;
                }
                if (wParam == VK_DELETE) {
                    if (selectAll) {
                        field.clear();
                        cursor = 0;
                        selectAll = false;
                    } else if (cursor < field.size()) {
                        field.erase(cursor, 1);
                    }
                    RequestDraw();
                    return 0;
                }
            } else if (wParam == VK_SPACE) {
                if (focus_ == FocusItem::Close) Close();
                return 0;
            }
            break;
        }
        case WM_CHAR:
            if (HasEditableField()) {
                std::wstring& field = FocusedText();
                std::size_t& cursor = FocusedCursor();
                bool& selectAll = FocusedSelectAll();
                if (wParam == VK_BACK) {
                    if (selectAll) {
                        field.clear();
                        cursor = 0;
                        selectAll = false;
                    } else if (cursor) {
                        field.erase(--cursor, 1);
                    }
                    RequestDraw();
                } else if (wParam >= 0x20) {
                    const wchar_t character = static_cast<wchar_t>(wParam);
                    InsertText(std::wstring_view(&character, 1));
                }
            }
            return 0;
        case WM_CLOSE:
            Close();
            return 0;
        case WM_DESTROY:
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
    BitToolDialogResult result_{};
    const UiStrings* strings_{};
    const Profile* profile_{};
    Palette palette_{};
    D2DDialogSurface surface_{};
    std::wstring inputText_;
    std::wstring operandText_;
    std::size_t inputCursor_{};
    std::size_t operandCursor_{};
    FocusItem focus_{FocusItem::Operations};
    HoverItem hover_{HoverItem::None};
    bool running_{true};
    bool ready_{};
    bool renderQueued_{};
    bool caretVisible_{true};
    bool inputSelectAll_{};
    bool operandSelectAll_{};
    bool applyingTheme_{};
};
}

BitToolDialogResult ShowBitToolDialog(HWND owner, HINSTANCE instance,
                                      const BitToolDialogResult& initial,
                                      UiLanguage language,
                                      const Profile& profile) {
    // Stack lifetime is safe because Show does not return until WM_DESTROY.
    BitToolWindow window(owner, instance, initial, language, profile);
    return window.Show();
}
