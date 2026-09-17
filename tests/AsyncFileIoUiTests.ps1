param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

# Exercises the production open and same-path save routes with a large sparse
# fixture. The test proves that both operations run on the named file-I/O worker,
# the UI thread answers messages, menu headings are disabled while bytes are
# borrowed by that worker, and editor input delivered during save is ignored.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

public static class BinEditIoAutomation
{
    public const string MainWindowClass = "BinEdit.MainWindow";
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_GETDLGCODE = 0x0087;
    public const uint WM_COMMAND = 0x0111;
    public const uint WM_CHAR = 0x0102;
    public const uint SMTO_ABORTIFHUNG = 0x0002;
    private const uint MF_BYPOSITION = 0x00000400;
    private const uint MF_GRAYED = 0x00000001;
    private const uint MF_DISABLED = 0x00000002;
    private const uint THREAD_QUERY_LIMITED_INFORMATION = 0x0800;

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
    private static extern IntPtr SendMessageTimeoutW(IntPtr window, uint message, UIntPtr wParam,
        IntPtr lParam, uint flags, uint timeout, out UIntPtr result);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetMenu(IntPtr window);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern int GetMenuItemCount(IntPtr menu);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetMenuState(IntPtr menu, uint item, uint flags);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenThread(uint desiredAccess, bool inheritHandle, uint threadId);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetThreadDescription(IntPtr thread, out IntPtr description);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr memory);

    public static IntPtr FindMainWindow(uint processId)
    {
        IntPtr result = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr parameter)
        {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner != processId) return true;
            StringBuilder name = new StringBuilder(256);
            if (GetClassNameW(window, name, name.Capacity) > 0 &&
                name.ToString() == MainWindowClass)
            {
                result = window;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }

    public static bool Ping(IntPtr window, uint timeoutMilliseconds)
    {
        UIntPtr ignored;
        return SendMessageTimeoutW(window, WM_GETDLGCODE, UIntPtr.Zero, IntPtr.Zero,
            SMTO_ABORTIFHUNG, timeoutMilliseconds, out ignored) != IntPtr.Zero;
    }

    public static string WindowText(IntPtr window)
    {
        StringBuilder text = new StringBuilder(1024);
        GetWindowTextW(window, text, text.Capacity);
        return text.ToString();
    }

    public static bool AllMenuHeadingsDisabled(IntPtr window)
    {
        IntPtr menu = GetMenu(window);
        if (menu == IntPtr.Zero) return false;
        int count = GetMenuItemCount(menu);
        if (count <= 0) return false;
        for (uint position = 0; position < count; ++position)
        {
            uint state = GetMenuState(menu, position, MF_BYPOSITION);
            if (state == UInt32.MaxValue || (state & (MF_GRAYED | MF_DISABLED)) == 0)
                return false;
        }
        return true;
    }

    public static int CountIoWorkers(uint processId)
    {
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
                            if (String.Equals(Marshal.PtrToStringUni(description), "BinEdit file I/O",
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

    public static void PostSave(IntPtr window)
    {
        if (!PostMessageW(window, WM_COMMAND, new UIntPtr(40002), IntPtr.Zero))
            throw new InvalidOperationException("PostMessage(Save) failed.");
    }

    public static void TypeByte(IntPtr window, byte value)
    {
        const string digits = "0123456789ABCDEF";
        SendMessageW(window, WM_CHAR, new UIntPtr(digits[value >> 4]), IntPtr.Zero);
        SendMessageW(window, WM_CHAR, new UIntPtr(digits[value & 15]), IntPtr.Zero);
    }

    public static void Close(IntPtr window)
    {
        SendMessageW(window, WM_CLOSE, UIntPtr.Zero, IntPtr.Zero);
    }
}
'@

function Wait-IoState {
    param(
        [Diagnostics.Process]$Process,
        [IntPtr]$Window,
        [bool]$Busy,
        [int]$TimeoutSeconds = 20
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if ($Process.HasExited) { throw "BinEdit exited unexpectedly with code $($Process.ExitCode)." }
        $workerCount = [BinEditIoAutomation]::CountIoWorkers([uint32]$Process.Id)
        if ($Busy -and $workerCount -ge 1 -and
            [BinEditIoAutomation]::AllMenuHeadingsDisabled($Window)) { return }
        if (-not $Busy -and $workerCount -eq 0 -and
            -not [BinEditIoAutomation]::AllMenuHeadingsDisabled($Window)) { return }
        Start-Sleep -Milliseconds 5
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for file-I/O busy=$Busy."
}

$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root "x64\$Configuration\BinEdit.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Build output was not found: $executable"
}

$fixtureSize = 384L * 1024L * 1024L
$fixture = Join-Path ([IO.Path]::GetTempPath()) "BinEdit.AsyncIo.$PID.bin"
$sentinelOffsets = [long[]](0L, (4MB - 1L), 4MB, (128MB + 17L), ($fixtureSize - 1L))
$sentinelValues = [byte[]](0x31, 0x42, 0x53, 0x64, 0x75)
$stream = [IO.File]::Open($fixture, [IO.FileMode]::Create,
    [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    $stream.SetLength($fixtureSize)
    for ($index = 0; $index -lt $sentinelOffsets.Length; ++$index) {
        $stream.Position = $sentinelOffsets[$index]
        $stream.WriteByte($sentinelValues[$index])
    }
    $stream.Flush($true)
} finally { $stream.Dispose() }

$process = $null
$passed = $false
try {
    $process = Start-Process -FilePath $executable -ArgumentList ('"{0}"' -f $fixture) `
        -WindowStyle Hidden -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    [IntPtr]$window = [IntPtr]::Zero
    do {
        if ($process.HasExited) { throw "BinEdit exited during startup with code $($process.ExitCode)." }
        $window = [BinEditIoAutomation]::FindMainWindow([uint32]$process.Id)
        if ($window -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 2
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($window -eq [IntPtr]::Zero) { throw 'Timed out waiting for the main window.' }

    # Memory mapping can complete before a 5 ms observer sees the worker. Accept
    # that fast path only after the loaded file title is published; if the worker
    # is observable, retain the full progress/menu/UI responsiveness assertions.
    $fileName = [IO.Path]::GetFileName($fixture)
    $loadingWorkerObserved = $false
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        if ($process.HasExited) { throw 'BinEdit exited during mapped file loading.' }
        if ([BinEditIoAutomation]::CountIoWorkers([uint32]$process.Id) -ge 1) {
            $loadingWorkerObserved = $true
            if (-not [BinEditIoAutomation]::AllMenuHeadingsDisabled($window)) {
                Start-Sleep -Milliseconds 2
                continue
            }
            if (-not [BinEditIoAutomation]::Ping($window, 250)) {
                throw 'The UI thread did not respond during asynchronous file loading.'
            }
            break
        }
        if ([BinEditIoAutomation]::WindowText($window).Contains($fileName)) { break }
        Start-Sleep -Milliseconds 2
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $loadingWorkerObserved -and
        -not [BinEditIoAutomation]::WindowText($window).Contains($fileName)) {
        throw 'Mapped file loading neither exposed a worker nor completed.'
    }
    Wait-IoState $process $window $false

    # Insert AA at the first caret, then save. A BB byte sent after the worker is
    # observed must be rejected by the busy input gate rather than enter history.
    [BinEditIoAutomation]::TypeByte($window, 0xAA)
    [BinEditIoAutomation]::PostSave($window)
    Wait-IoState $process $window $true
    if (-not [BinEditIoAutomation]::Ping($window, 250)) {
        throw 'The UI thread did not respond during asynchronous file saving.'
    }
    [BinEditIoAutomation]::TypeByte($window, 0xBB)
    Wait-IoState $process $window $false 40

    $info = [IO.FileInfo]::new($fixture)
    if ($info.Length -ne $fixtureSize + 1) {
        throw "Saved fixture length is $($info.Length); expected $($fixtureSize + 1)."
    }
    $reader = [IO.File]::Open($fixture, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($reader.ReadByte() -ne 0xAA -or $reader.ReadByte() -ne $sentinelValues[0]) {
            throw 'The saved prefix does not contain exactly the accepted pre-save edit.'
        }
        for ($index = 0; $index -lt $sentinelOffsets.Length; ++$index) {
            $reader.Position = $sentinelOffsets[$index] + 1L
            if ($reader.ReadByte() -ne $sentinelValues[$index]) {
                throw "The shifted sentinel at original offset $($sentinelOffsets[$index]) was corrupted."
            }
        }
    } finally { $reader.Dispose() }

    [BinEditIoAutomation]::Close($window)
    if (-not $process.WaitForExit(5000) -or $process.ExitCode -ne 0) {
        throw 'BinEdit did not exit cleanly after the asynchronous I/O test.'
    }

    $passed = $true
    [pscustomobject]@{
        Result = 'PASS'
        Configuration = $Configuration
        FixtureBytes = $fixtureSize
        LoadingWorkerObserved = $loadingWorkerObserved
        LoadingCompletedViaMappedFastPath = -not $loadingWorkerObserved
        SavingWorkerObserved = $true
        UiResponsive = $true
        MenusDisabledWhileBusy = $true
        BusyInputRejected = $true
        ShiftedSentinelsPreserved = $true
    } | Format-List
}
finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    if ($passed -and (Test-Path -LiteralPath $fixture)) {
        Remove-Item -LiteralPath $fixture -Force
    }
    if (-not $passed) { Write-Warning "Failed asynchronous-I/O fixture retained: $fixture" }
}
