#pragma once

// Strongly typed localization contract shared by every UI surface.
// LanguagePreference is persisted; UiLanguage is the resolved language used
// for the current session after applying the Windows UI-language fallback.

#include <windows.h>

// Resolved language used to index an immutable string table.
enum class UiLanguage { Japanese, English };
// Saved choice. System is intentionally distinct from its current resolution.
enum class LanguagePreference { System, Japanese, English };

// Every user-visible phrase owned by the application. Native common dialogs and
// FormatMessage text are requested separately in the corresponding language.
struct UiStrings {
    // Main window and top-level menus.
    const wchar_t* untitled;
    const wchar_t* menuFile;
    const wchar_t* fileNew;
    const wchar_t* fileOpen;
    const wchar_t* fileSave;
    const wchar_t* fileSaveAs;
    const wchar_t* fileClose;
    const wchar_t* fileDetachTab;
    const wchar_t* fileExit;
    const wchar_t* menuEdit;
    const wchar_t* editUndo;
    const wchar_t* editRedo;
    const wchar_t* editFind;
    const wchar_t* editFindNext;
    const wchar_t* editMark;
    const wchar_t* editClearMarks;
    const wchar_t* menuTools;
    const wchar_t* toolsBitOperation;
    const wchar_t* menuEncoding;
    const wchar_t* menuColor;
    const wchar_t* colorMark;
    const wchar_t* colorByte;
    const wchar_t* colorClearByte;
    const wchar_t* menuTheme;
    const wchar_t* themeSystem;
    const wchar_t* themeLight;
    const wchar_t* themeDark;
    const wchar_t* menuLanguage;
    const wchar_t* languageSystem;
    const wchar_t* languageJapanese;
    const wchar_t* languageEnglish;
    const wchar_t* menuHelp;
    const wchar_t* helpAbout;
    // Main D2D editor headings and status template.
    const wchar_t* headerOffset;
    const wchar_t* headerCharacters;
    const wchar_t* statusFormat;
    const wchar_t* inputHexadecimal;
    const wchar_t* inputCharacters;
    const wchar_t* editModeInsert;
    const wchar_t* editModeOverwrite;
    const wchar_t* statusSearching;
    const wchar_t* statusLoadingFile;
    const wchar_t* statusSavingFile;
    // File, initialization, and elevation workflow messages.
    const wchar_t* initializeFailed;
    const wchar_t* instanceCoordinationFailed;
    const wchar_t* saveChanges;
    const wchar_t* allFiles;
    const wchar_t* openFailed;
    const wchar_t* saveFailed;
    const wchar_t* stageFailed;
    const wchar_t* elevationPrompt;
    const wchar_t* elevatedTargetPrompt;
    const wchar_t* elevatedSaveFailed;
    const wchar_t* shutdownBlockReason;
    const wchar_t* recoveryTitle;
    const wchar_t* recoveryPrompt;
    const wchar_t* externalChangePrompt;
    const wchar_t* externalFileMissing;
    // Shared labels for the application-owned GDI message dialog.
    const wchar_t* dialogOk;
    const wchar_t* dialogYes;
    const wchar_t* dialogNo;
    const wchar_t* dialogCancel;
    const wchar_t* dialogReload;
    const wchar_t* dialogContinue;
    // Search dialog controls and validation messages.
    const wchar_t* searchTitle;
    const wchar_t* searchQuery;
    const wchar_t* searchBinary;
    const wchar_t* searchText;
    const wchar_t* searchEncoding;
    const wchar_t* searchButton;
    const wchar_t* cancelButton;
    const wchar_t* searchNotFound;
    const wchar_t* searchFailed;
    const wchar_t* searchEmpty;
    const wchar_t* searchInvalidHex;
    const wchar_t* searchSeparator;
    const wchar_t* searchUnrepresentable;
    const wchar_t* searchEncodingFailed;
    const wchar_t* searchTooLarge;
    const wchar_t* searchAllocationFailed;
    // Bit-operation dialog controls, guidance, validation, and operation names.
    const wchar_t* bitToolTitle;
    const wchar_t* bitToolOperation;
    const wchar_t* bitToolInput;
    const wchar_t* bitToolInputHint;
    const wchar_t* bitToolOperand;
    const wchar_t* bitToolOperandHint;
    const wchar_t* bitToolPreview;
    const wchar_t* bitToolClose;
    const wchar_t* bitToolWrapHint;
    const wchar_t* bitToolInvalidInput;
    const wchar_t* bitToolInvalidOperand;
    const wchar_t* bitToolBitCountRange;
    const wchar_t* bitToolDivideByZero;
    const wchar_t* bitOpAnd;
    const wchar_t* bitOpOr;
    const wchar_t* bitOpXor;
    const wchar_t* bitOpNot;
    const wchar_t* bitOpShiftLeft;
    const wchar_t* bitOpShiftRight;
    const wchar_t* bitOpRotateLeft;
    const wchar_t* bitOpRotateRight;
    const wchar_t* bitOpAdd;
    const wchar_t* bitOpSubtract;
    const wchar_t* bitOpMultiply;
    const wchar_t* bitOpDivide;
    // Custom color-picker labels and input validation.
    const wchar_t* colorPickerTitle;
    const wchar_t* colorPickerSpectrum;
    const wchar_t* colorPickerCurrent;
    const wchar_t* colorPickerNew;
    const wchar_t* colorPickerRed;
    const wchar_t* colorPickerGreen;
    const wchar_t* colorPickerBlue;
    const wchar_t* colorPickerHex;
    const wchar_t* colorPickerHint;
    const wchar_t* colorPickerApply;
    const wchar_t* colorPickerInvalidNumber;
    const wchar_t* colorPickerInvalidHex;
    // About dialog title and body.
    const wchar_t* aboutTitle;
    const wchar_t* aboutBody;
    const wchar_t* aboutVersionLabel;
    const wchar_t* aboutBuildLabel;
    const wchar_t* aboutBuildDateLabel;
};

// Resolves System using the user's Windows UI language, not the formatting
// locale. All non-Japanese UI languages intentionally fall back to English.
UiLanguage ResolveLanguage(LanguagePreference preference);
// Returns a process-lifetime constexpr table; callers must not free its strings.
const UiStrings& GetStrings(UiLanguage language);
