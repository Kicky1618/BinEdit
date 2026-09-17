#pragma once

// Stable process-wide names shared by startup routing, window creation, and
// the explicitly versioned single-instance copy-data protocol.

#include <windows.h>

namespace BinEditIdentity {
inline constexpr wchar_t MainWindowClass[] = L"BinEdit.MainWindow";
inline constexpr wchar_t DetachedWindowClass[] = L"BinEdit.DetachedTabWindow";
inline constexpr wchar_t InstanceMutex[] = L"MX_BinEdit_F80F";
inline constexpr ULONG_PTR OpenFilesCopyDataId = 0x504F4542u; // MAKEFOURCC('B', 'E', 'O', 'P').
}
