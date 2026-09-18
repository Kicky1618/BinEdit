param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

# Drives the custom Direct2D search dialog and the real editor HWND. The first
# phase scans a large sparse file with a never-matching anchored pattern to
# prove that the scan owns multiple worker threads while the UI thread remains
# responsive. The second phase verifies that asynchronous completion and Find
# Next select the expected byte by saving a deterministic edit at that position.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

public static class BinEditSearchAutomation
{
    public const string MainWindowClass = "BinEdit.MainWindow";
    public const string SearchWindowClass = "BinEdit.SearchDialog";
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_GETDLGCODE = 0x0087;
    public const uint WM_COMMAND = 0x0111;
    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_SYSKEYDOWN = 0x0104;
    public const uint WM_CHAR = 0x0102;
    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP = 0x0202;
    public const int VK_RETURN = 0x0D;
    public const int VK_BACK = 0x08;
    public const int VK_F3 = 0x72;
    public const uint SMTO_ABORTIFHUNG = 0x0002;

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

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SendMessageTimeoutW(
        IntPtr window,
        uint message,
        UIntPtr wParam,
        IntPtr lParam,
        uint flags,
        uint timeout,
        out UIntPtr result);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenThread(uint desiredAccess, bool inheritHandle, uint threadId);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetThreadDescription(IntPtr thread, out IntPtr description);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr memory);

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

    public static void Command(IntPtr window, int command)
    {
        SendMessageW(window, WM_COMMAND, new UIntPtr((uint)command), IntPtr.Zero);
    }

    public static void Key(IntPtr window, int virtualKey)
    {
        SendMessageW(window, WM_KEYDOWN, new UIntPtr((uint)virtualKey), IntPtr.Zero);
    }

    public static void SystemKey(IntPtr window, int virtualKey)
    {
        SendMessageW(window, WM_SYSKEYDOWN, new UIntPtr((uint)virtualKey), IntPtr.Zero);
    }

    public static void ClickLogical(IntPtr window, int x, int y)
    {
        // SendMessage originates in the DPI-unaware PowerShell host. Use the same
        // logical coordinates accepted by the custom-dialog automation path and
        // do not apply the monitor scale a second time.
        IntPtr packed = new IntPtr((y << 16) | (x & 0xffff));
        SendMessageW(window, WM_LBUTTONDOWN, new UIntPtr(1), packed);
        SendMessageW(window, WM_LBUTTONUP, UIntPtr.Zero, packed);
    }

    public static void TypeText(IntPtr window, string text)
    {
        foreach (char character in text)
            SendMessageW(window, WM_CHAR, new UIntPtr(character), IntPtr.Zero);
    }

    public static void TypeByte(IntPtr window, byte value)
    {
        const string digits = "0123456789ABCDEF";
        SendMessageW(window, WM_CHAR, new UIntPtr(digits[value >> 4]), IntPtr.Zero);
        SendMessageW(window, WM_CHAR, new UIntPtr(digits[value & 15]), IntPtr.Zero);
    }

    public static bool Ping(IntPtr window, uint timeoutMilliseconds)
    {
        UIntPtr ignored;
        return SendMessageTimeoutW(window, WM_GETDLGCODE, UIntPtr.Zero, IntPtr.Zero,
            SMTO_ABORTIFHUNG, timeoutMilliseconds, out ignored) != IntPtr.Zero;
    }

    public static int CountNamedThreads(uint processId, string expectedDescription)
    {
        const uint THREAD_QUERY_LIMITED_INFORMATION = 0x0800;
        int count = 0;
        using (Process process = Process.GetProcessById((int)processId))
        {
            foreach (ProcessThread thread in process.Threads)
            {
                IntPtr handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, false, (uint)thread.Id);
                if (handle == IntPtr.Zero) continue;
                try
                {
                    IntPtr description;
                    if (GetThreadDescription(handle, out description) >= 0 && description != IntPtr.Zero)
                    {
                        try
                        {
                            if (String.Equals(Marshal.PtrToStringUni(description), expectedDescription,
                                StringComparison.Ordinal)) ++count;
                        }
                        finally { LocalFree(description); }
                    }
                }
                finally { CloseHandle(handle); }
            }
        }
        return count;
    }
}
'@

function Wait-BinEditWindow {
    param(
        [Diagnostics.Process]$Process,
        [string]$ClassName,
        [bool]$Present = $true,
        [int]$TimeoutSeconds = 15
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if ($Process.HasExited) { throw "BinEdit exited unexpectedly with code $($Process.ExitCode)." }
        [IntPtr]$window = [BinEditSearchAutomation]::FindWindow([uint32]$Process.Id, $ClassName)
        if ($Present -and $window -ne [IntPtr]::Zero) { return $window }
        if (-not $Present -and $window -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
        Start-Sleep -Milliseconds 20
    } while ([DateTime]::UtcNow -lt $deadline)
    $state = if ($Present) { 'appear' } else { 'close' }
    throw "Timed out waiting for window class '$ClassName' to $state."
}

function Start-BinEditFixture {
    param([string]$Executable, [string]$Path)
    $process = Start-Process -FilePath $Executable -ArgumentList ('"{0}"' -f $Path) `
        -WindowStyle Hidden -PassThru
    $mainWindow = Wait-BinEditWindow $process ([BinEditSearchAutomation]::MainWindowClass)
    $fileName = [IO.Path]::GetFileName($Path)
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $deadline -and
           -not [BinEditSearchAutomation]::WindowText($mainWindow).Contains($fileName)) {
        Start-Sleep -Milliseconds 10
    }
    if (-not [BinEditSearchAutomation]::WindowText($mainWindow).Contains($fileName)) {
        throw "Timed out waiting for '$fileName' to finish loading."
    }
    [pscustomobject]@{ Process = $process; Window = $mainWindow }
}

function Open-SearchAndSubmit {
    param(
        [Diagnostics.Process]$Process,
        [IntPtr]$MainWindow,
        [string]$Query,
        [ref]$DialogThreadCount
    )

    # WM_COMMAND is posted because the handler owns a nested modal message loop.
    [BinEditSearchAutomation]::PostCommand($MainWindow, 40103)
    [IntPtr]$dialog = Wait-BinEditWindow $Process ([BinEditSearchAutomation]::SearchWindowClass)
    $Process.Refresh()
    $DialogThreadCount.Value = $Process.Threads.Count
    [BinEditSearchAutomation]::TypeText($dialog, $Query)
    [BinEditSearchAutomation]::Key($dialog, [BinEditSearchAutomation]::VK_RETURN)
    $null = Wait-BinEditWindow $Process ([BinEditSearchAutomation]::SearchWindowClass) $false
}

function Wait-MultithreadedSearch {
    param(
        [Diagnostics.Process]$Process,
        [int]$TimeoutSeconds = 10
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $maximum = 0
    do {
        if ($Process.HasExited) { throw "BinEdit exited unexpectedly with code $($Process.ExitCode)." }
        $workers = [BinEditSearchAutomation]::CountNamedThreads(
            [uint32]$Process.Id, 'BinEdit parallel search')
        $maximum = [Math]::Max($maximum, $workers)
        if ($workers -ge 2) { return $maximum }
        Start-Sleep -Milliseconds 5
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Multiple named parallel-search workers were not observed (maximum $maximum)."
}

function Assert-Bytes {
    param([string]$Path, [byte[]]$Expected, [string]$Label)
    $actual = [IO.File]::ReadAllBytes($Path)
    if ($actual.Length -ne $Expected.Length) {
        throw "$Label has $($actual.Length) bytes; expected $($Expected.Length)."
    }
    for ($index = 0; $index -lt $actual.Length; ++$index) {
        if ($actual[$index] -ne $Expected[$index]) {
            throw ('{0} mismatch at 0x{1:X}: actual {2:X2}, expected {3:X2}.' -f
                $Label, $index, $actual[$index], $Expected[$index])
        }
    }
}

$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root "x64\$Configuration\BinEdit.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Build output was not found: $executable" }

$parallelPath = Join-Path ([IO.Path]::GetTempPath()) "BinEdit.SearchAsync.$PID.parallel.bin"
$selectionPath = Join-Path ([IO.Path]::GetTempPath()) "BinEdit.SearchAsync.$PID.selection.bin"
$parallelProcess = $null
$selectionProcess = $null
$passed = $false

try {
    # A sparse zero-filled file keeps fixture setup cheap. The anchor is chosen
    # from a byte-frequency sample, so a query whose literal bytes are absent
    # from the file forces a complete scan of every partition and keeps the
    # worker pool observable until the window is closed. Dense patterns (all
    # wildcards or frequent anchors) stop once the highlight cap is full.
    $stream = [IO.File]::Open($parallelPath, [IO.FileMode]::Create,
        [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.SetLength(256MB) } finally { $stream.Dispose() }
    $parallelRun = Start-BinEditFixture $executable $parallelPath
    $parallelProcess = $parallelRun.Process
    [IntPtr]$parallelWindow = $parallelRun.Window
    $baselineThreads = 0
    $wildcardQuery = 'DE AD ?? 00'
    Open-SearchAndSubmit $parallelProcess $parallelWindow $wildcardQuery ([ref]$baselineThreads)
    $maximumWorkers = Wait-MultithreadedSearch $parallelProcess

    $pingTimer = [Diagnostics.Stopwatch]::StartNew()
    $responsive = [BinEditSearchAutomation]::Ping($parallelWindow, 250)
    $pingTimer.Stop()
    if (-not $responsive) { throw 'The main window did not respond while parallel search was running.' }
    $workersBeforeClose = [BinEditSearchAutomation]::CountNamedThreads(
        [uint32]$parallelProcess.Id, 'BinEdit parallel search')
    if ($workersBeforeClose -lt 1) {
        throw 'The expensive search completed before the active-cancellation check could run.'
    }

    # Closing while the expensive scan is active exercises cooperative stop,
    # worker joins, and destruction without retaining document/window pointers.
    [BinEditSearchAutomation]::PostClose($parallelWindow)
    if (-not $parallelProcess.WaitForExit(5000)) {
        throw 'BinEdit did not exit promptly while canceling an active parallel search.'
    }
    if ($parallelProcess.ExitCode -ne 0) {
        throw "Parallel-search process exited with code $($parallelProcess.ExitCode)."
    }

    # Two unique occurrences make F3 selection observable: the byte inserted
    # after completion must land at the second occurrence, not at the old caret.
    [byte[]]$selectionBytes = [byte[]]::new(8192)
    [byte[]]$textNeedle = [Text.Encoding]::ASCII.GetBytes('TEXTMODE')
    [byte[]]$needle = 0xDE, 0xAD, 0xBE, 0xEF
    [Array]::Copy($textNeedle, 0, $selectionBytes, 256, $textNeedle.Length)
    [Array]::Copy($needle, 0, $selectionBytes, 1024, $needle.Length)
    [Array]::Copy($needle, 0, $selectionBytes, 6144, $needle.Length)
    [IO.File]::WriteAllBytes($selectionPath, $selectionBytes)

    $selectionRun = Start-BinEditFixture $executable $selectionPath
    $selectionProcess = $selectionRun.Process
    [IntPtr]$selectionWindow = $selectionRun.Window

    # The custom radio is clicked at its DPI-scaled drawn rectangle, then the
    # query field is re-focused. TEXTMODE is deliberately invalid binary syntax,
    # so the dialog can close only if mouse selection really changed to text.
    [BinEditSearchAutomation]::PostCommand($selectionWindow, 40103)
    [IntPtr]$textDialog = Wait-BinEditWindow $selectionProcess ([BinEditSearchAutomation]::SearchWindowClass)
    [BinEditSearchAutomation]::ClickLogical($textDialog, 254, 112)
    [BinEditSearchAutomation]::ClickLogical($textDialog, 100, 62)
    [BinEditSearchAutomation]::TypeText($textDialog, 'TEXTMODE')
    [BinEditSearchAutomation]::Key($textDialog, [BinEditSearchAutomation]::VK_RETURN)
    $null = Wait-BinEditWindow $selectionProcess ([BinEditSearchAutomation]::SearchWindowClass) $false
    Start-Sleep -Milliseconds 300

    # The accepted mode/query are retained on reopen. Exercise explicit Alt+B and
    # Alt+Q handling, erase the previous query, and return to a binary pattern.
    # If switching failed, the text search would not find the raw DE AD bytes and
    # the later byte-for-byte selection assertion would fail.
    [BinEditSearchAutomation]::PostCommand($selectionWindow, 40103)
    [IntPtr]$binaryDialog = Wait-BinEditWindow $selectionProcess ([BinEditSearchAutomation]::SearchWindowClass)
    [BinEditSearchAutomation]::SystemKey($binaryDialog, [int][char]'B')
    [BinEditSearchAutomation]::SystemKey($binaryDialog, [int][char]'Q')
    1..$textNeedle.Length | ForEach-Object {
        [BinEditSearchAutomation]::TypeText($binaryDialog, [string][char][BinEditSearchAutomation]::VK_BACK)
    }
    [BinEditSearchAutomation]::TypeText($binaryDialog, 'DE AD BE EF')
    [BinEditSearchAutomation]::Key($binaryDialog, [BinEditSearchAutomation]::VK_RETURN)
    $null = Wait-BinEditWindow $selectionProcess ([BinEditSearchAutomation]::SearchWindowClass) $false

    $unusedBaseline = 0
    Start-Sleep -Milliseconds 500
    [BinEditSearchAutomation]::Key($selectionWindow, [BinEditSearchAutomation]::VK_F3)
    Start-Sleep -Milliseconds 500
    [BinEditSearchAutomation]::TypeByte($selectionWindow, 0xCC)
    [BinEditSearchAutomation]::Command($selectionWindow, 40002)

    $expected = [Collections.Generic.List[byte]]::new()
    $expected.AddRange($selectionBytes)
    $expected.Insert(6144, 0xCC)
    Assert-Bytes $selectionPath $expected.ToArray() 'asynchronous Find Next edit'

    $passed = $true
    [pscustomobject]@{
        Result = 'PASS'
        Configuration = $Configuration
        SearchFixtureMiB = 256
        DialogBaselineThreads = $baselineThreads
        NamedWorkersObserved = $maximumWorkers
        MultipleWorkersObserved = $true
        UiResponsiveDuringSearch = $true
        UiPingMilliseconds = $pingTimer.ElapsedMilliseconds
        WorkersBeforeClose = $workersBeforeClose
        ActiveSearchCanceledOnExit = $true
        FindNextSelectionOffset = 6144
        TextModeMouseSwitchAccepted = $true
        BinaryModeMnemonicSwitchAccepted = $true
        ByteForByteMatch = $true
    } | Format-List
}
finally {
    foreach ($process in @($selectionProcess, $parallelProcess)) {
        if ($process -and -not $process.HasExited) {
            [IntPtr]$window = [BinEditSearchAutomation]::FindWindow(
                [uint32]$process.Id, [BinEditSearchAutomation]::MainWindowClass)
            if ($window -ne [IntPtr]::Zero) {
                [BinEditSearchAutomation]::PostClose($window)
            }
            $null = $process.WaitForExit(3000)
            if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
        }
    }
    if ($passed) {
        if (Test-Path -LiteralPath $parallelPath) { Remove-Item -LiteralPath $parallelPath -Force }
        if (Test-Path -LiteralPath $selectionPath) { Remove-Item -LiteralPath $selectionPath -Force }
    } else {
        Write-Warning "Failed asynchronous-search fixtures retained: $parallelPath ; $selectionPath"
    }
}
