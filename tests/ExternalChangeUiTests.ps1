param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

# Exercises external-change handling through the real top-level HWND. The test
# covers automatic clean reload, explicit dirty Continue, explicit dirty Reload,
# and the byte sequence ultimately committed after each decision.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class BinEditExternalChangeAutomation
{
    public const string MainWindowClass = "BinEdit.MainWindow";
    public const string MessageWindowClass = "BinEdit.MessageDialog.Gdi";
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_COMMAND = 0x0111;
    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_KEYUP = 0x0101;
    public const uint WM_CHAR = 0x0102;
    public const int VK_RETURN = 0x0D;
    public const int VK_RIGHT = 0x27;
    public const int VK_INSERT = 0x2D;
    public const int VK_SHIFT = 0x10;
    public const int VK_TAB = 0x09;
    private const uint MF_BYPOSITION = 0x00000400;
    private const uint MF_GRAYED = 0x00000001;
    private const uint MF_DISABLED = 0x00000002;

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

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool PostMessageW(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetMenu(IntPtr window);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern int GetMenuItemCount(IntPtr menu);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetMenuState(IntPtr menu, uint item, uint flags);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool ReplaceFileW(string replacedFileName,
        string replacementFileName, string backupFileName, uint replaceFlags,
        IntPtr exclude, IntPtr reserved);

    public static IntPtr FindWindow(uint processId, string expectedClass)
    {
        IntPtr result = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr parameter)
        {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (processId != 0 && owner != processId) return true;
            StringBuilder name = new StringBuilder(256);
            if (GetClassNameW(window, name, name.Capacity) > 0 && name.ToString() == expectedClass)
            {
                result = window;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }

    public static string WindowText(IntPtr window)
    {
        StringBuilder text = new StringBuilder(1024);
        GetWindowTextW(window, text, text.Capacity);
        return text.ToString();
    }

    public static bool MenuHeadingsEnabled(IntPtr window)
    {
        IntPtr menu = GetMenu(window);
        int count = menu == IntPtr.Zero ? 0 : GetMenuItemCount(menu);
        if (count <= 0) return false;
        for (uint position = 0; position < count; ++position)
        {
            uint state = GetMenuState(menu, position, MF_BYPOSITION);
            if (state == UInt32.MaxValue || (state & (MF_GRAYED | MF_DISABLED)) != 0)
                return false;
        }
        return true;
    }

    public static void Key(IntPtr window, int virtualKey)
    {
        SendMessageW(window, WM_KEYDOWN, new UIntPtr((uint)virtualKey), IntPtr.Zero);
    }

    public static void KeyUp(IntPtr window, int virtualKey)
    {
        SendMessageW(window, WM_KEYUP, new UIntPtr((uint)virtualKey), IntPtr.Zero);
    }

    public static void TypeByte(IntPtr window, byte value)
    {
        const string digits = "0123456789ABCDEF";
        SendMessageW(window, WM_CHAR, new UIntPtr(digits[value >> 4]), IntPtr.Zero);
        SendMessageW(window, WM_CHAR, new UIntPtr(digits[value & 15]), IntPtr.Zero);
    }

    public static void PostCommand(IntPtr window, int command)
    {
        if (!PostMessageW(window, WM_COMMAND, new UIntPtr((uint)command), IntPtr.Zero))
            throw new InvalidOperationException("PostMessage(WM_COMMAND) failed.");
    }

    public static void PostClose(IntPtr window)
    {
        if (!PostMessageW(window, WM_CLOSE, UIntPtr.Zero, IntPtr.Zero))
            throw new InvalidOperationException("PostMessage(WM_CLOSE) failed.");
    }

    public static void PublishReplacement(string destination, string replacement)
    {
        if (!ReplaceFileW(destination, replacement, null, 2u, IntPtr.Zero, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error());
    }
}
'@

function Wait-BinEditWindow {
    param(
        [Diagnostics.Process]$Process,
        [string]$ClassName,
        [int]$TimeoutSeconds = 15
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if ($Process.HasExited) { throw "BinEdit exited unexpectedly with code $($Process.ExitCode)." }
        [IntPtr]$window = [BinEditExternalChangeAutomation]::FindWindow([uint32]$Process.Id, $ClassName)
        if ($window -ne [IntPtr]::Zero) { return $window }
        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for window class '$ClassName'."
}

function Wait-NoMessageDialog {
    param([Diagnostics.Process]$Process, [int]$Milliseconds)
    $deadline = [DateTime]::UtcNow.AddMilliseconds($Milliseconds)
    do {
        if ($Process.HasExited) { throw "BinEdit exited unexpectedly with code $($Process.ExitCode)." }
        if ([BinEditExternalChangeAutomation]::FindWindow([uint32]$Process.Id,
            [BinEditExternalChangeAutomation]::MessageWindowClass) -ne [IntPtr]::Zero) {
            throw 'An external-change dialog appeared for a clean document.'
        }
        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $deadline)
}

function Wait-EditorReady {
    param([Diagnostics.Process]$Process, [IntPtr]$Window, [int]$TimeoutSeconds = 15)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if ($Process.HasExited) { throw "BinEdit exited unexpectedly with code $($Process.ExitCode)." }
        if ([BinEditExternalChangeAutomation]::MenuHeadingsEnabled($Window)) { return }
        Start-Sleep -Milliseconds 10
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Timed out waiting for the editor I/O operation to finish.'
}

function Wait-Bytes {
    param([string]$Path, [byte[]]$Expected, [int]$TimeoutSeconds = 15)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        try {
            $actual = [IO.File]::ReadAllBytes($Path)
            if ($actual.Length -eq $Expected.Length) {
                $equal = $true
                for ($index = 0; $index -lt $actual.Length; ++$index) {
                    if ($actual[$index] -ne $Expected[$index]) { $equal = $false; break }
                }
                if ($equal) { return }
            }
        } catch [IO.IOException] {
            # A save may briefly deny sharing while flushing its partial write.
        }
        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Timed out waiting for the expected file bytes.'
}

function Publish-ExternalBytes {
    param([string]$Path, [byte[]]$Bytes)

    # A mapped data section intentionally pins the existing stream length, so
    # File.WriteAllBytes cannot truncate that same file object. Real editors
    # commonly publish through a sibling and atomic replacement. This also
    # verifies that BinEdit's mapped view remains a stable old-file snapshot
    # until its directory watcher accepts or rejects the replacement.
    $temporary = $Path + '.external.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    try {
        [IO.File]::WriteAllBytes($temporary, $Bytes)
        [BinEditExternalChangeAutomation]::PublishReplacement($Path, $temporary)
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root "x64\$Configuration\BinEdit.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Build output was not found: $executable"
}
if ([BinEditExternalChangeAutomation]::FindWindow(0,
    [BinEditExternalChangeAutomation]::MainWindowClass) -ne [IntPtr]::Zero) {
    throw 'Close the existing BinEdit window before running the external-change UI test.'
}

$fixture = Join-Path ([IO.Path]::GetTempPath()) "BinEdit.ExternalChangeUi.$PID.bin"
[byte[]]$initial = 0x10, 0x20, 0x30, 0x40
[IO.File]::WriteAllBytes($fixture, $initial)
$process = $null
$passed = $false

try {
    $process = Start-Process -FilePath $executable -ArgumentList ('"{0}"' -f $fixture) `
        -WindowStyle Hidden -PassThru
    [IntPtr]$editor = Wait-BinEditWindow $process ([BinEditExternalChangeAutomation]::MainWindowClass)
    $fixtureName = [IO.Path]::GetFileName($fixture)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline -and
           -not [BinEditExternalChangeAutomation]::WindowText($editor).Contains($fixtureName)) {
        Start-Sleep -Milliseconds 10
    }
    if (-not [BinEditExternalChangeAutomation]::WindowText($editor).Contains($fixtureName)) {
        throw 'Timed out waiting for the command-line fixture to finish loading.'
    }

    # A clean external update must reload without a confirmation dialog. An
    # overwrite based on the reloaded bytes makes the result observable on disk.
    [byte[]]$cleanExternal = 0x55, 0x66, 0x77, 0x88
    Publish-ExternalBytes $fixture $cleanExternal
    Wait-NoMessageDialog $process 1800
    [BinEditExternalChangeAutomation]::Key($editor, [BinEditExternalChangeAutomation]::VK_INSERT)
    [BinEditExternalChangeAutomation]::TypeByte($editor, 0xAA)
    [BinEditExternalChangeAutomation]::PostCommand($editor, 40002)
    [byte[]]$afterCleanReload = 0xAA, 0x66, 0x77, 0x88
    Wait-Bytes $fixture $afterCleanReload

    # Keep the local dirty version when the external process changes a different
    # byte. Continue is the final custom-dialog button; Shift+Tab must wrap focus
    # backward from the default Reload button before Enter activates it.
    [BinEditExternalChangeAutomation]::TypeByte($editor, 0xBB)
    [byte[]]$dirtyExternal = 0xAA, 0x66, 0xCC, 0x88
    Publish-ExternalBytes $fixture $dirtyExternal
    [IntPtr]$continuePrompt = Wait-BinEditWindow $process ([BinEditExternalChangeAutomation]::MessageWindowClass)
    [BinEditExternalChangeAutomation]::Key($continuePrompt, [BinEditExternalChangeAutomation]::VK_SHIFT)
    [BinEditExternalChangeAutomation]::Key($continuePrompt, [BinEditExternalChangeAutomation]::VK_TAB)
    [BinEditExternalChangeAutomation]::KeyUp($continuePrompt, [BinEditExternalChangeAutomation]::VK_SHIFT)
    [BinEditExternalChangeAutomation]::Key($continuePrompt, [BinEditExternalChangeAutomation]::VK_RETURN)
    [BinEditExternalChangeAutomation]::PostCommand($editor, 40002)
    [byte[]]$afterContinue = 0xAA, 0xBB, 0x77, 0x88
    Wait-Bytes $fixture $afterContinue

    # The default first button is Reload. It must discard the local dirty byte,
    # reset the editing session, and use the newly published external sequence.
    [BinEditExternalChangeAutomation]::TypeByte($editor, 0xDD)
    [byte[]]$reloadExternal = 0x11, 0x22, 0x33, 0x44
    Publish-ExternalBytes $fixture $reloadExternal
    [IntPtr]$reloadPrompt = Wait-BinEditWindow $process ([BinEditExternalChangeAutomation]::MessageWindowClass)
    [BinEditExternalChangeAutomation]::Key($reloadPrompt, [BinEditExternalChangeAutomation]::VK_RETURN)
    # The dialog closes before its owning timer handler has necessarily entered
    # the asynchronous reload. Give that continuation one scheduling turn, then
    # use the disabled-menu contract as the authoritative completion barrier.
    Start-Sleep -Milliseconds 100
    Wait-EditorReady $process $editor
    [BinEditExternalChangeAutomation]::Key($editor, [BinEditExternalChangeAutomation]::VK_INSERT)
    [BinEditExternalChangeAutomation]::TypeByte($editor, 0xEE)
    [BinEditExternalChangeAutomation]::PostCommand($editor, 40002)
    [byte[]]$afterReload = 0xEE, 0x22, 0x33, 0x44
    Wait-Bytes $fixture $afterReload

    [BinEditExternalChangeAutomation]::PostClose($editor)
    if (-not $process.WaitForExit(5000) -or $process.ExitCode -ne 0) {
        throw 'BinEdit did not exit cleanly after the external-change test.'
    }

    $passed = $true
    [pscustomobject]@{
        Result = 'PASS'
        Configuration = $Configuration
        CleanChangeAutoReloaded = $true
        DirtyContinuePreservedLocalBytes = $true
        ShiftTabSelectedContinue = $true
        DirtyReloadAcceptedExternalBytes = $true
        ByteForByteMatch = $true
    } | Format-List
}
finally {
    if ($process -and -not $process.HasExited) {
        [IntPtr]$window = [BinEditExternalChangeAutomation]::FindWindow(
            [uint32]$process.Id, [BinEditExternalChangeAutomation]::MainWindowClass)
        if ($window -ne [IntPtr]::Zero) { [BinEditExternalChangeAutomation]::PostClose($window) }
        $null = $process.WaitForExit(3000)
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    }
    if ($passed -and (Test-Path -LiteralPath $fixture)) { Remove-Item -LiteralPath $fixture -Force }
    if (-not $passed) { Write-Warning "Failed external-change fixture retained: $fixture" }
}
