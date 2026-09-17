param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

# Verifies the exact TerminateProcess-style failure mode used by taskkill /F.
# Two dirty documents are split across the main and sub windows before forced
# termination. The next main window must aggregate both records as tabs, without
# recreating a sub window, and save each byte-exact recovered document.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class BinEditRecoveryAutomation
{
    public const string MainWindowClass = "BinEdit.MainWindow";
    public const string DetachedWindowClass = "BinEdit.DetachedTabWindow";
    public const string MessageWindowClass = "BinEdit.MessageDialog.Gdi";
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_COMMAND = 0x0111;
    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_CHAR = 0x0102;
    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP = 0x0202;
    public const uint WM_MOUSEMOVE = 0x0200;
    public const int VK_RETURN = 0x0D;

    private delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }

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

    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr window);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool GetClientRect(IntPtr window, out RECT rect);

    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool PostMessageW(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);

    public static IntPtr FindWindow(uint processId, string expectedClass)
    {
        IntPtr result = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr parameter)
        {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner != processId) return true;
            StringBuilder name = new StringBuilder(256);
            int length = GetClassNameW(window, name, name.Capacity);
            if (length > 0 && name.ToString() == expectedClass)
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

    public static void Command(IntPtr window, int command)
    {
        SendMessageW(window, WM_COMMAND, new UIntPtr((uint)command), IntPtr.Zero);
    }

    public static void Key(IntPtr window, int virtualKey)
    {
        SendMessageW(window, WM_KEYDOWN, new UIntPtr((uint)virtualKey), IntPtr.Zero);
    }

    public static void TypeByte(IntPtr window, byte value)
    {
        const string digits = "0123456789ABCDEF";
        SendMessageW(window, WM_CHAR, new UIntPtr(digits[value >> 4]), IntPtr.Zero);
        SendMessageW(window, WM_CHAR, new UIntPtr(digits[value & 15]), IntPtr.Zero);
    }

    public static void SendPoint(IntPtr window, uint message, ulong wParam, int x, int y)
    {
        long packed = ((long)(ushort)(short)y << 16) | (ushort)(short)x;
        SendMessageW(window, message, new UIntPtr(wParam), new IntPtr(packed));
    }

    public static void PostClose(IntPtr window)
    {
        if (!PostMessageW(window, WM_CLOSE, UIntPtr.Zero, IntPtr.Zero))
            throw new InvalidOperationException("PostMessage(WM_CLOSE) failed.");
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
        [IntPtr]$window = [BinEditRecoveryAutomation]::FindWindow([uint32]$Process.Id, $ClassName)
        if ($window -ne [IntPtr]::Zero) { return $window }
        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for window class '$ClassName'."
}

function Wait-NewRecoveryRecord {
    param([string]$Directory, [string[]]$Before)
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $current = @(Get-ChildItem -LiteralPath $Directory -Filter '*.binrecovery' -File -Force -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty FullName)
        $newRecord = @($current | Where-Object { $_ -notin $Before })
        if ($newRecord.Count -eq 1) {
            $item = Get-Item -LiteralPath $newRecord[0] -Force -ErrorAction SilentlyContinue
            if ($item -and $item.Length -ge 64) { return $newRecord[0] }
        }
        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Timed out waiting for a complete recovery record.'
}

function Assert-Bytes {
    param([string]$Path, [byte[]]$Expected)
    $actual = [IO.File]::ReadAllBytes($Path)
    if ($actual.Length -ne $Expected.Length) {
        throw "Recovered file has $($actual.Length) bytes; expected $($Expected.Length)."
    }
    for ($index = 0; $index -lt $actual.Length; ++$index) {
        if ($actual[$index] -ne $Expected[$index]) {
            throw ('Recovered file mismatch at 0x{0:X}: actual {1:X2}, expected {2:X2}.' -f
                $index, $actual[$index], $Expected[$index])
        }
    }
}

function Get-TabPoint {
    param([IntPtr]$Window, [int]$Index, [int]$Count)

    $rect = [BinEditRecoveryAutomation+RECT]::new()
    if (-not [BinEditRecoveryAutomation]::GetClientRect($Window, [ref]$rect)) {
        throw 'GetClientRect failed.'
    }
    $scale = [BinEditRecoveryAutomation]::GetDpiForWindow($Window) / 96.0
    $margin = [Math]::Round(8.0 * $scale)
    $available = ($rect.Right - $rect.Left) - $margin * 2
    $width = [Math]::Max([Math]::Round(56.0 * $scale),
        [Math]::Min([Math]::Round(220.0 * $scale), $available / $Count))
    [pscustomobject]@{
        X = [int]($margin + $Index * $width + [Math]::Min($width * 0.3, 36.0 * $scale))
        Y = [int][Math]::Round(20.0 * $scale)
    }
}

function Click-Tab {
    param([IntPtr]$Window, [int]$Index, [int]$Count)
    $point = Get-TabPoint $Window $Index $Count
    [BinEditRecoveryAutomation]::SendPoint(
        $Window, [BinEditRecoveryAutomation]::WM_LBUTTONDOWN, 1, $point.X, $point.Y)
    [BinEditRecoveryAutomation]::SendPoint(
        $Window, [BinEditRecoveryAutomation]::WM_LBUTTONUP, 0, $point.X, $point.Y)
}

function Wait-WindowClosed {
    param([IntPtr]$Window, [int]$TimeoutSeconds = 10)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([BinEditRecoveryAutomation]::IsWindow($Window)) {
        if ([DateTime]::UtcNow -ge $deadline) { throw 'A recovery prompt did not close.' }
        Start-Sleep -Milliseconds 25
    }
}

$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root "x64\$Configuration\BinEdit.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Build output was not found: $executable" }
$recoveryDirectory = Join-Path $env:LOCALAPPDATA 'BinEdit\Recovery'
$existing = @(Get-ChildItem -LiteralPath $recoveryDirectory -Filter '*.binrecovery' -File -Force -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty FullName)
if ($existing.Count -ne 0) {
    throw 'Recovery test refused to run because pre-existing user recovery records must not be modified.'
}

$mainFixture = Join-Path ([IO.Path]::GetTempPath()) "BinEdit.RecoveryUi.$PID.main.bin"
$subFixture = Join-Path ([IO.Path]::GetTempPath()) "BinEdit.RecoveryUi.$PID.sub.bin"
[byte[]]$mainInitial = 0x11, 0x22, 0x33, 0x44
[byte[]]$subInitial = 0x55, 0x66, 0x77
[byte[]]$mainExpected = 0xCC, 0x11, 0x22, 0x33, 0x44
[byte[]]$subExpected = 0xDD, 0x55, 0x66, 0x77
[IO.File]::WriteAllBytes($mainFixture, $mainInitial)
[IO.File]::WriteAllBytes($subFixture, $subInitial)
$first = $null
$second = $null
$records = @()
$passed = $false

try {
    $arguments = @(('"{0}"' -f $mainFixture), ('"{0}"' -f $subFixture))
    $first = Start-Process -FilePath $executable -ArgumentList $arguments -WindowStyle Hidden -PassThru
    [IntPtr]$firstWindow = Wait-BinEditWindow $first ([BinEditRecoveryAutomation]::MainWindowClass)
    # Startup file opens are asynchronous and the final command-line file is the
    # active tab. Its title is the stable barrier before tab hit testing begins.
    $activeFileName = [IO.Path]::GetFileName($subFixture)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline -and
           -not [BinEditRecoveryAutomation]::WindowText($firstWindow).Contains($activeFileName)) {
        Start-Sleep -Milliseconds 10
    }
    if (-not [BinEditRecoveryAutomation]::WindowText($firstWindow).Contains($activeFileName)) {
        throw 'Timed out waiting for both command-line files to finish loading.'
    }

    # Startup selects the second file. Detach it so each top-level controller
    # owns one dirty document when the process is force-terminated.
    $detachPoint = Get-TabPoint $firstWindow 1 2
    [BinEditRecoveryAutomation]::SendPoint(
        $firstWindow, [BinEditRecoveryAutomation]::WM_LBUTTONDOWN, 1,
        $detachPoint.X, $detachPoint.Y)
    [BinEditRecoveryAutomation]::SendPoint(
        $firstWindow, [BinEditRecoveryAutomation]::WM_MOUSEMOVE, 1,
        $detachPoint.X, $detachPoint.Y + 120)
    [BinEditRecoveryAutomation]::SendPoint(
        $firstWindow, [BinEditRecoveryAutomation]::WM_LBUTTONUP, 0,
        $detachPoint.X, $detachPoint.Y + 120)
    [IntPtr]$subWindow = Wait-BinEditWindow $first ([BinEditRecoveryAutomation]::DetachedWindowClass)

    [BinEditRecoveryAutomation]::TypeByte($firstWindow, 0xCC)
    $mainRecord = Wait-NewRecoveryRecord $recoveryDirectory $existing
    # The high nibble creates the first immediate checkpoint. Wait beyond the
    # bounded idle interval so the low nibble's newer generation replaces it.
    Start-Sleep -Milliseconds 1000
    if (-not (Test-Path -LiteralPath $mainRecord)) {
        throw 'The latest main-window recovery generation was not committed.'
    }
    $beforeSub = @(Get-ChildItem -LiteralPath $recoveryDirectory -Filter '*.binrecovery' `
        -File -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
    [BinEditRecoveryAutomation]::TypeByte($subWindow, 0xDD)
    $subRecord = Wait-NewRecoveryRecord $recoveryDirectory $beforeSub
    Start-Sleep -Milliseconds 1000
    if (-not (Test-Path -LiteralPath $subRecord)) {
        throw 'The latest sub-window recovery generation was not committed.'
    }
    $records = @($mainRecord, $subRecord)

    # Stop-Process -Force maps to TerminateProcess, so no destructor, WM_CLOSE,
    # shutdown block callback, or jthread join is allowed to run in this phase.
    Stop-Process -Id $first.Id -Force
    if (-not $first.WaitForExit(5000) -or -not $first.HasExited) {
        throw 'Force-terminated BinEdit did not reach the signaled process state.'
    }

    $second = Start-Process -FilePath $executable -WindowStyle Hidden -PassThru
    [IntPtr]$secondWindow = Wait-BinEditWindow $second ([BinEditRecoveryAutomation]::MainWindowClass)
    for ($promptIndex = 0; $promptIndex -lt 2; ++$promptIndex) {
        [IntPtr]$prompt = Wait-BinEditWindow $second ([BinEditRecoveryAutomation]::MessageWindowClass)
        [BinEditRecoveryAutomation]::Key($prompt, [BinEditRecoveryAutomation]::VK_RETURN)
        Wait-WindowClosed $prompt
    }

    if ([BinEditRecoveryAutomation]::FindWindow([uint32]$second.Id,
        [BinEditRecoveryAutomation]::DetachedWindowClass) -ne [IntPtr]::Zero) {
        throw 'Recovery recreated a sub window instead of aggregating tabs in the main window.'
    }

    # Chronological recovery makes the sub-window record the active second tab.
    [BinEditRecoveryAutomation]::Command($secondWindow, 40002)
    Assert-Bytes $subFixture $subExpected
    Click-Tab $secondWindow 0 2
    [BinEditRecoveryAutomation]::Command($secondWindow, 40002)
    Assert-Bytes $mainFixture $mainExpected

    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (@($records | Where-Object { Test-Path -LiteralPath $_ }).Count -ne 0) {
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Recovery record remained after a successful save.' }
        Start-Sleep -Milliseconds 25
    }
    [BinEditRecoveryAutomation]::PostClose($secondWindow)
    if (-not $second.WaitForExit(5000) -or $second.ExitCode -ne 0) {
        throw 'Recovered BinEdit instance did not exit cleanly.'
    }

    $passed = $true
    [pscustomobject]@{
        Result = 'PASS'
        Configuration = $Configuration
        ForcedTermination = $true
        ProcessFullyTerminated = $true
        RecoveryRecordsCreated = $records.Count
        RecoveryPromptsAccepted = 2
        RecoveredOnlyIntoMainWindow = $true
        RecoveredBytes = $mainExpected.Length + $subExpected.Length
        RecoveryRemovedAfterSave = $true
        ByteForByteMatch = $true
    } | Format-List
}
finally {
    foreach ($process in @($second, $first)) {
        if ($process -and -not $process.HasExited) {
            [IntPtr]$window = [BinEditRecoveryAutomation]::FindWindow(
                [uint32]$process.Id, [BinEditRecoveryAutomation]::MainWindowClass)
            if ($window -ne [IntPtr]::Zero) { [BinEditRecoveryAutomation]::PostClose($window) }
            $null = $process.WaitForExit(3000)
            if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
        }
    }
    if ($passed) {
        foreach ($fixture in @($mainFixture, $subFixture)) {
            if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Force }
        }
    }
    if (-not $passed) {
        Write-Warning "Failed recovery fixtures/records retained: $mainFixture ; $subFixture ; $($records -join ' ; ')"
    }
}
