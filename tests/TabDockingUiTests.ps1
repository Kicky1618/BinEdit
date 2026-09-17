param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

# Exercises the named-mutex forwarding route, primary/detached HWND class split,
# multi-document tab isolation, and same-process detach/dock behavior through the
# real message path. Fixtures are removed only after every saved byte and expected
# top-level-window transition has been verified.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class BinEditTabAutomation
{
    public const string MainWindowClass = "BinEdit.MainWindow";
    public const string DetachedWindowClass = "BinEdit.DetachedTabWindow";
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_COMMAND = 0x0111;
    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_CHAR = 0x0102;
    public const uint WM_INITMENUPOPUP = 0x0117;
    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP = 0x0202;
    public const uint WM_MOUSEMOVE = 0x0200;
    public const int VK_HOME = 0x24;
    private const uint MF_BYCOMMAND = 0x00000000;
    private const uint MF_GRAYED = 0x00000001;
    private const uint MF_DISABLED = 0x00000002;

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X; public int Y; }

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    private struct GUITHREADINFO
    {
        public uint cbSize;
        public uint flags;
        public IntPtr hwndActive;
        public IntPtr hwndFocus;
        public IntPtr hwndCapture;
        public IntPtr hwndMenuOwner;
        public IntPtr hwndMoveSize;
        public IntPtr hwndCaret;
        public RECT rcCaret;
    }

    private delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetClassNameW(IntPtr window, StringBuilder className, int maximumCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetWindowTextW(IntPtr window, StringBuilder text, int maximumCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessageW(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetMenu(IntPtr window);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetSubMenu(IntPtr menu, int position);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetMenuState(IntPtr menu, uint item, uint flags);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern int GetMenuItemCount(IntPtr menu);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetGUIThreadInfo(uint threadId, ref GUITHREADINFO information);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool GetClientRect(IntPtr window, out RECT rect);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool ClientToScreen(IntPtr window, ref POINT point);

    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr window);

    public static IntPtr[] FindEditorWindows(uint processId)
    {
        List<IntPtr> result = new List<IntPtr>();
        EnumWindows(delegate(IntPtr window, IntPtr parameter)
        {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner != processId) return true;
            StringBuilder name = new StringBuilder(256);
            int length = GetClassNameW(window, name, name.Capacity);
            string className = length > 0 ? name.ToString() : String.Empty;
            if (className == MainWindowClass || className == DetachedWindowClass) result.Add(window);
            return true;
        }, IntPtr.Zero);
        return result.ToArray();
    }

    public static string ClassName(IntPtr window)
    {
        StringBuilder name = new StringBuilder(256);
        int length = GetClassNameW(window, name, name.Capacity);
        return length > 0 ? name.ToString() : String.Empty;
    }

    public static string WindowText(IntPtr window)
    {
        StringBuilder text = new StringBuilder(1024);
        GetWindowTextW(window, text, text.Capacity);
        return text.ToString();
    }

    public static void Send(IntPtr window, uint message, ulong wParam, int x, int y)
    {
        long packed = ((long)(ushort)(short)y << 16) | (ushort)(short)x;
        SendMessageW(window, message, new UIntPtr(wParam), new IntPtr(packed));
    }

    public static void Command(IntPtr window, int command)
    {
        SendMessageW(window, WM_COMMAND, new UIntPtr((uint)command), IntPtr.Zero);
    }

    public static void Key(IntPtr window, int virtualKey)
    {
        SendMessageW(window, WM_KEYDOWN, new UIntPtr((uint)virtualKey), IntPtr.Zero);
    }

    public static bool FileCommandEnabled(IntPtr window, uint command)
    {
        IntPtr menu = GetMenu(window);
        IntPtr fileMenu = GetSubMenu(menu, 0);
        if (menu == IntPtr.Zero || fileMenu == IntPtr.Zero)
            throw new InvalidOperationException("Could not resolve the File menu.");
        SendMessageW(window, WM_INITMENUPOPUP, new UIntPtr(unchecked((ulong)fileMenu.ToInt64())), IntPtr.Zero);
        uint state = GetMenuState(fileMenu, command, MF_BYCOMMAND);
        if (state == UInt32.MaxValue)
            throw new InvalidOperationException("Could not resolve File command " + command + ".");
        return (state & (MF_GRAYED | MF_DISABLED)) == 0;
    }

    public static bool FileCommandExists(IntPtr window, uint command)
    {
        IntPtr menu = GetMenu(window);
        IntPtr fileMenu = GetSubMenu(menu, 0);
        if (menu == IntPtr.Zero || fileMenu == IntPtr.Zero)
            throw new InvalidOperationException("Could not resolve the File menu.");
        return GetMenuState(fileMenu, command, MF_BYCOMMAND) != UInt32.MaxValue;
    }

    public static int TopLevelMenuCount(IntPtr window)
    {
        IntPtr menu = GetMenu(window);
        if (menu == IntPtr.Zero) throw new InvalidOperationException("Could not resolve the menu bar.");
        return GetMenuItemCount(menu);
    }

    public static IntPtr ThreadFocus(IntPtr window)
    {
        uint ignored;
        uint threadId = GetWindowThreadProcessId(window, out ignored);
        GUITHREADINFO information = new GUITHREADINFO();
        information.cbSize = (uint)Marshal.SizeOf(typeof(GUITHREADINFO));
        if (threadId == 0 || !GetGUIThreadInfo(threadId, ref information))
            throw new InvalidOperationException("GetGUIThreadInfo failed.");
        return information.hwndFocus;
    }

    public static void TypeByte(IntPtr window, byte value)
    {
        const string digits = "0123456789ABCDEF";
        SendMessageW(window, WM_CHAR, new UIntPtr(digits[value >> 4]), IntPtr.Zero);
        SendMessageW(window, WM_CHAR, new UIntPtr(digits[value & 15]), IntPtr.Zero);
    }
}
'@

function Get-TabPoint {
    param([IntPtr]$Window, [int]$Index, [int]$Count)

    $rect = [BinEditTabAutomation+RECT]::new()
    if (-not [BinEditTabAutomation]::GetClientRect($Window, [ref]$rect)) { throw 'GetClientRect failed.' }
    $scale = [BinEditTabAutomation]::GetDpiForWindow($Window) / 96.0
    $margin = [Math]::Round(8.0 * $scale)
    $available = ($rect.Right - $rect.Left) - $margin * 2
    $width = [Math]::Max([Math]::Round(56.0 * $scale),
        [Math]::Min([Math]::Round(220.0 * $scale), $available / $Count))
    [pscustomobject]@{
        X = [int]($margin + $Index * $width + [Math]::Min($width * 0.3, 36.0 * $scale))
        Y = [int][Math]::Round(20.0 * $scale)
        Width = [double]$width
        Margin = [double]$margin
        ClientWidth = $rect.Right - $rect.Left
    }
}

function Click-Tab {
    param([IntPtr]$Window, [int]$Index, [int]$Count)
    $point = Get-TabPoint $Window $Index $Count
    [BinEditTabAutomation]::Send($Window, [BinEditTabAutomation]::WM_LBUTTONDOWN, 1, $point.X, $point.Y)
    [BinEditTabAutomation]::Send($Window, [BinEditTabAutomation]::WM_LBUTTONUP, 0, $point.X, $point.Y)
}

function Wait-WindowCount {
    param([Diagnostics.Process]$Process, [int]$Count)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    $lastCount = -1
    do {
        if ($Process.HasExited) { throw "BinEdit exited unexpectedly with code $($Process.ExitCode)." }
        $windows = [BinEditTabAutomation]::FindEditorWindows([uint32]$Process.Id)
        $lastCount = $windows.Length
        if ($windows.Length -eq $Count) { return $windows }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for $Count BinEdit window(s); last observed count was $lastCount."
}

function Wait-ActiveFile {
    param([Diagnostics.Process]$Process, [IntPtr]$Window, [string]$Path)
    $fileName = [IO.Path]::GetFileName($Path)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        if ($Process.HasExited) { throw "BinEdit exited while opening '$fileName'." }
        if ([BinEditTabAutomation]::WindowText($Window).Contains($fileName)) { return }
        Start-Sleep -Milliseconds 10
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for '$fileName' to become the active loaded tab."
}

function Assert-Bytes {
    param([string]$Path, [byte[]]$Expected, [string]$Label)
    $actual = [IO.File]::ReadAllBytes($Path)
    if ($actual.Length -ne $Expected.Length) { throw "$Label has $($actual.Length) bytes; expected $($Expected.Length)." }
    for ($index = 0; $index -lt $actual.Length; ++$index) {
        if ($actual[$index] -ne $Expected[$index]) {
            throw ('{0} mismatch at 0x{1:X}: actual {2:X2}, expected {3:X2}.' -f
                $Label, $index, $actual[$index], $Expected[$index])
        }
    }
}

function Assert-WindowClass {
    param([IntPtr]$Window, [string]$Expected, [string]$Label)
    $actual = [BinEditTabAutomation]::ClassName($Window)
    if ($actual -ne $Expected) { throw "$Label uses window class '$actual'; expected '$Expected'." }
}

function Assert-FileCommandEnabled {
    param([IntPtr]$Window, [uint32]$Command, [bool]$Expected, [string]$Label)
    $actual = [BinEditTabAutomation]::FileCommandEnabled($Window, $Command)
    if ($actual -ne $Expected) {
        throw "$Label enabled state was $actual; expected $Expected."
    }
}

$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root "x64\$Configuration\BinEdit.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Build output was not found: $executable" }

$firstPath = Join-Path ([IO.Path]::GetTempPath()) "BinEdit.TabDock.$PID.first.bin"
$secondPath = Join-Path ([IO.Path]::GetTempPath()) "BinEdit.TabDock.$PID.second.bin"
$forwardedPath = Join-Path ([IO.Path]::GetTempPath()) "BinEdit TabDock $PID 転送テスト.bin"
[byte[]]$firstExpected = 0x11, 0x22, 0x33
[byte[]]$secondExpected = 0xAA, 0xBB
[byte[]]$forwardedExpected = 0x71, 0x72
[IO.File]::WriteAllBytes($firstPath, $firstExpected)
[IO.File]::WriteAllBytes($secondPath, $secondExpected)
[IO.File]::WriteAllBytes($forwardedPath, $forwardedExpected)
$process = $null
$secondary = $null
$passed = $false

try {
    $arguments = @(('"{0}"' -f $firstPath), ('"{0}"' -f $secondPath))
    $process = Start-Process -FilePath $executable -ArgumentList $arguments -WindowStyle Hidden -PassThru
    [IntPtr]$rootWindow = (Wait-WindowCount $process 1)[0]
    Wait-ActiveFile $process $rootWindow $secondPath
    Assert-WindowClass $rootWindow ([BinEditTabAutomation]::MainWindowClass) 'primary host'
    Assert-FileCommandEnabled $rootWindow 40005 $true 'two-tab Close tab command'
    Assert-FileCommandEnabled $rootWindow 40006 $true 'two-tab Move tab command'
    $instanceMutex = $null
    try {
        $instanceMutex = [Threading.Mutex]::OpenExisting('MX_BinEdit_F80F')
    }
    catch {
        throw 'Named mutex MX_BinEdit_F80F was not observable while the primary instance was running.'
    }
    finally {
        if ($instanceMutex) { $instanceMutex.Dispose() }
    }

    # A second process must hand its command-line file to the existing primary
    # HWND through the named-mutex/WM_COPYDATA route and terminate without ever
    # creating another main-class window.
    $forwardArgument = '"{0}"' -f $forwardedPath
    $secondary = Start-Process -FilePath $executable -ArgumentList $forwardArgument -WindowStyle Hidden -PassThru
    if (-not $secondary.WaitForExit(15000)) { throw 'Secondary BinEdit instance did not terminate after forwarding.' }
    if ($secondary.ExitCode -ne 0) { throw "Secondary BinEdit instance exited with code $($secondary.ExitCode)." }
    [IntPtr]$rootWindow = (Wait-WindowCount $process 1)[0]
    Wait-ActiveFile $process $rootWindow $forwardedPath
    Assert-WindowClass $rootWindow ([BinEditTabAutomation]::MainWindowClass) 'primary host after forwarding'
    [BinEditTabAutomation]::TypeByte($rootWindow, 0xEE)
    [BinEditTabAutomation]::Command($rootWindow, 40002)
    [byte[]]$forwardedExpected = 0xEE, 0x71, 0x72
    Assert-Bytes $forwardedPath $forwardedExpected 'forwarded third tab'
    [BinEditTabAutomation]::Command($rootWindow, 40005)

    # The last command-line file is active. Insert and save there first.
    [BinEditTabAutomation]::TypeByte($rootWindow, 0xCC)
    [BinEditTabAutomation]::Command($rootWindow, 40002)
    [byte[]]$secondExpected = 0xCC, 0xAA, 0xBB
    Assert-Bytes $secondPath $secondExpected 'second tab before detach'

    Click-Tab $rootWindow 0 2
    [BinEditTabAutomation]::TypeByte($rootWindow, 0x44)
    [BinEditTabAutomation]::Command($rootWindow, 40002)
    [byte[]]$firstExpected = 0x44, 0x11, 0x22, 0x33
    Assert-Bytes $firstPath $firstExpected 'first tab before detach'

    # Move the active first-file tab after the second tab. Editing tab zero after
    # that gesture proves both the visual order and document-state association.
    $reorderFrom = Get-TabPoint $rootWindow 0 2
    $reorderX = [int]($reorderFrom.Margin + $reorderFrom.Width * 2 - 10)
    [BinEditTabAutomation]::Send($rootWindow, [BinEditTabAutomation]::WM_LBUTTONDOWN, 1,
        $reorderFrom.X, $reorderFrom.Y)
    [BinEditTabAutomation]::Send($rootWindow, [BinEditTabAutomation]::WM_MOUSEMOVE, 1,
        $reorderX, $reorderFrom.Y)
    [BinEditTabAutomation]::Send($rootWindow, [BinEditTabAutomation]::WM_LBUTTONUP, 0,
        $reorderX, $reorderFrom.Y)
    Start-Sleep -Milliseconds 100
    Click-Tab $rootWindow 0 2
    [BinEditTabAutomation]::Key($rootWindow, [BinEditTabAutomation]::VK_HOME)
    [BinEditTabAutomation]::TypeByte($rootWindow, 0xDD)
    [BinEditTabAutomation]::Command($rootWindow, 40002)
    [byte[]]$secondExpected = 0xDD, 0xCC, 0xAA, 0xBB
    Assert-Bytes $secondPath $secondExpected 'second tab after reorder'
    Click-Tab $rootWindow 1 2

    # Pull the active tab into the document body. The application interprets a
    # release outside its tab strip as a Visual Studio-style detach gesture.
    $detachTab = Get-TabPoint $rootWindow 1 2
    [BinEditTabAutomation]::Send($rootWindow, [BinEditTabAutomation]::WM_LBUTTONDOWN, 1,
        $detachTab.X, $detachTab.Y)
    [BinEditTabAutomation]::Send($rootWindow, [BinEditTabAutomation]::WM_MOUSEMOVE, 1,
        $detachTab.X, $detachTab.Y + 120)
    [BinEditTabAutomation]::Send($rootWindow, [BinEditTabAutomation]::WM_LBUTTONUP, 0,
        $detachTab.X, $detachTab.Y + 120)
    $windows = Wait-WindowCount $process 2
    [IntPtr]$childWindow = ($windows | Where-Object { $_ -ne $rootWindow })[0]
    Assert-WindowClass $rootWindow ([BinEditTabAutomation]::MainWindowClass) 'primary host after detach'
    Assert-WindowClass $childWindow ([BinEditTabAutomation]::DetachedWindowClass) 'detached host'
    Assert-FileCommandEnabled $rootWindow 40005 $true 'primary final named-tab Close tab command'
    Assert-FileCommandEnabled $rootWindow 40006 $false 'primary final-tab Move tab command'
    Assert-FileCommandEnabled $childWindow 40005 $true 'detached single-tab Close tab command'
    Assert-FileCommandEnabled $childWindow 40006 $false 'detached single-tab Move tab command'
    if ([BinEditTabAutomation]::TopLevelMenuCount($rootWindow) -ne 8) {
        throw 'The main window did not expose the complete process-level menu set.'
    }
    if ([BinEditTabAutomation]::TopLevelMenuCount($childWindow) -ne 6) {
        throw 'The sub window retained Theme or Language menus.'
    }
    if (-not [BinEditTabAutomation]::FileCommandExists($rootWindow, 40004) -or
        [BinEditTabAutomation]::FileCommandExists($childWindow, 40004)) {
        throw 'File -> Exit ownership did not remain exclusive to the main window.'
    }
    $focusDeadline = [DateTime]::UtcNow.AddSeconds(3)
    do {
        [IntPtr]$focusedWindow = [BinEditTabAutomation]::ThreadFocus($rootWindow)
        if ($focusedWindow -eq $childWindow) { break }
        Start-Sleep -Milliseconds 20
    } while ([DateTime]::UtcNow -lt $focusDeadline)
    if ($focusedWindow -ne $childWindow) {
        throw 'Keyboard focus returned to the source window after detaching a tab.'
    }

    # Drag the detached tab to the original window's tab strip. The release point
    # is just after its sole remaining tab, so docking appends the transferred tab.
    $childTab = Get-TabPoint $childWindow 0 1
    $rootTab = Get-TabPoint $rootWindow 0 1
    $rootOrigin = [BinEditTabAutomation+POINT]::new()
    if (-not [BinEditTabAutomation]::ClientToScreen($rootWindow, [ref]$rootOrigin)) { throw 'Root ClientToScreen failed.' }
    $childOrigin = [BinEditTabAutomation+POINT]::new()
    if (-not [BinEditTabAutomation]::ClientToScreen($childWindow, [ref]$childOrigin)) { throw 'Child ClientToScreen failed.' }
    $dockScreenX = [int]($rootOrigin.X + [Math]::Min($rootTab.ClientWidth - 20, $rootTab.Margin + $rootTab.Width + 20))
    $dockScreenY = [int]($rootOrigin.Y + $rootTab.Y)
    [BinEditTabAutomation]::Send($childWindow, [BinEditTabAutomation]::WM_LBUTTONDOWN, 1, $childTab.X, $childTab.Y)
    [BinEditTabAutomation]::Send($childWindow, [BinEditTabAutomation]::WM_MOUSEMOVE, 1,
        $dockScreenX - $childOrigin.X, $dockScreenY - $childOrigin.Y)
    [BinEditTabAutomation]::Send($childWindow, [BinEditTabAutomation]::WM_LBUTTONUP, 0,
        $dockScreenX - $childOrigin.X, $dockScreenY - $childOrigin.Y)
    [IntPtr]$rootWindow = (Wait-WindowCount $process 1)[0]
    Assert-WindowClass $rootWindow ([BinEditTabAutomation]::MainWindowClass) 'primary host after redock'
    Assert-FileCommandEnabled $rootWindow 40005 $true 'redocked two-tab Close tab command'
    Assert-FileCommandEnabled $rootWindow 40006 $true 'redocked two-tab Move tab command'

    # Docking activates the transferred first file and preserves its caret/history.
    [BinEditTabAutomation]::TypeByte($rootWindow, 0x55)
    [BinEditTabAutomation]::Command($rootWindow, 40002)
    [byte[]]$firstExpected = 0x44, 0x55, 0x11, 0x22, 0x33
    Assert-Bytes $firstPath $firstExpected 'first tab after redock'
    Assert-Bytes $secondPath $secondExpected 'second tab after redock'

    # Recreate a sub host and close its sole tab. A sub window is only a detached
    # tab host, so its final-tab close must destroy that HWND while the main host
    # and process remain alive.
    $cascadeTab = Get-TabPoint $rootWindow 1 2
    [BinEditTabAutomation]::Send($rootWindow, [BinEditTabAutomation]::WM_LBUTTONDOWN, 1,
        $cascadeTab.X, $cascadeTab.Y)
    [BinEditTabAutomation]::Send($rootWindow, [BinEditTabAutomation]::WM_MOUSEMOVE, 1,
        $cascadeTab.X, $cascadeTab.Y + 120)
    [BinEditTabAutomation]::Send($rootWindow, [BinEditTabAutomation]::WM_LBUTTONUP, 0,
        $cascadeTab.X, $cascadeTab.Y + 120)
    $windows = Wait-WindowCount $process 2
    [IntPtr]$childWindow = ($windows | Where-Object { $_ -ne $rootWindow })[0]
    [BinEditTabAutomation]::Command($childWindow, 40005)
    $null = Wait-WindowCount $process 1
    if ($process.HasExited) { throw 'Closing a sub window final tab terminated the main process.' }

    # Create a clean second tab, detach it, then close only the main HWND. The
    # main window owns process lifetime, so its normal close transaction must
    # synchronously close the sub window and allow the message loop to exit.
    [BinEditTabAutomation]::Command($rootWindow, 40000)
    $cascadeTab = Get-TabPoint $rootWindow 1 2
    [BinEditTabAutomation]::Send($rootWindow, [BinEditTabAutomation]::WM_LBUTTONDOWN, 1,
        $cascadeTab.X, $cascadeTab.Y)
    [BinEditTabAutomation]::Send($rootWindow, [BinEditTabAutomation]::WM_MOUSEMOVE, 1,
        $cascadeTab.X, $cascadeTab.Y + 120)
    [BinEditTabAutomation]::Send($rootWindow, [BinEditTabAutomation]::WM_LBUTTONUP, 0,
        $cascadeTab.X, $cascadeTab.Y + 120)
    $null = Wait-WindowCount $process 2
    [BinEditTabAutomation]::Send($rootWindow, [BinEditTabAutomation]::WM_CLOSE, 0, 0, 0)
    if (-not $process.WaitForExit(5000) -or $process.ExitCode -ne 0) {
        throw 'Closing the main window did not close every sub window and exit cleanly.'
    }

    $passed = $true
    [pscustomobject]@{
        Result = 'PASS'
        Configuration = $Configuration
        InitialTabs = 2
        SecondLaunchForwarded = $true
        NamedMutexObserved = $true
        PrimaryWindowClassCount = 1
        DetachedWindowClassObserved = $true
        ReorderedTabs = $true
        DetachedByDrag = $true
        DetachedWindows = 2
        SubWindowMenusRestricted = $true
        DetachedSingleTabMoveDisabled = $true
        DetachedWindowRetainedFocus = $true
        SubFinalTabCloseDestroyedWindow = $true
        RedockedWindows = 1
        MainCloseCascadedToSubWindows = $true
        PrimaryFinalNamedTabCloseEnabled = $true
        FirstFileBytes = $firstExpected.Length
        SecondFileBytes = $secondExpected.Length
        ByteForByteMatch = $true
    } | Format-List
}
finally {
    if ($secondary -and -not $secondary.HasExited) { Stop-Process -Id $secondary.Id -Force }
    if ($process -and -not $process.HasExited) {
        $windows = [BinEditTabAutomation]::FindEditorWindows([uint32]$process.Id)
        foreach ($window in $windows) {
            [BinEditTabAutomation]::Send($window, [BinEditTabAutomation]::WM_CLOSE, 0, 0, 0)
        }
        $null = $process.WaitForExit(5000)
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    }
    if ($passed) {
        if (Test-Path -LiteralPath $firstPath) { Remove-Item -LiteralPath $firstPath -Force }
        if (Test-Path -LiteralPath $secondPath) { Remove-Item -LiteralPath $secondPath -Force }
        if (Test-Path -LiteralPath $forwardedPath) { Remove-Item -LiteralPath $forwardedPath -Force }
    } else {
        Write-Warning "Failed tab fixtures retained: $firstPath ; $secondPath ; $forwardedPath"
    }
}
