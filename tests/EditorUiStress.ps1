param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [int]$Seed = 20260915,
    [ValidateRange(32, 1048576)]
    [int]$TargetSize = 20 * 1024,
    [ValidateRange(1, 10000)]
    [int]$BackspaceCount = 256,
    [ValidateRange(1, 10000)]
    [int]$DeleteCount = 256,
    [ValidateRange(1, 10000)]
    [int]$OverwriteCount = 256
)

# Drives the real top-level editor HWND without depending on accessibility trees
# or native child controls. Every operation is delivered through the same window
# messages handled by a physical keyboard, and the saved file is then
# compared byte-for-byte with an independent in-memory oracle.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class BinEditEditorAutomation
{
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_COMMAND = 0x0111;
    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_CHAR = 0x0102;
    public const int VK_BACK = 0x08;
    public const int VK_INSERT = 0x2D;
    public const int VK_DELETE = 0x2E;
    public const int VK_LEFT = 0x25;
    public const int VK_UP = 0x26;
    public const int VK_RIGHT = 0x27;
    public const int VK_DOWN = 0x28;
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr FindWindowW(string className, string windowName);

    private delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetClassNameW(IntPtr window, StringBuilder className, int maximumCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetWindowTextW(IntPtr window, StringBuilder text, int maximumCount);

    public static IntPtr FindEditorWindow(uint processId)
    {
        IntPtr result = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr parameter)
        {
            uint ownerProcess;
            GetWindowThreadProcessId(window, out ownerProcess);
            if (ownerProcess != processId)
                return true;
            StringBuilder className = new StringBuilder(256);
            GetClassNameW(window, className, className.Capacity);
            if (className.ToString() != "BinEdit.MainWindow")
                return true;
            result = window;
            return false;
        }, IntPtr.Zero);
        return result;
    }

    public static string WindowText(IntPtr window)
    {
        StringBuilder text = new StringBuilder(1024);
        GetWindowTextW(window, text, text.Capacity);
        return text.ToString();
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessageW(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool PostMessageW(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);

    public static void Send(IntPtr window, uint message, ulong wParam, long lParam)
    {
        SendMessageW(window, message, new UIntPtr(wParam), new IntPtr(lParam));
    }

    public static void Post(IntPtr window, uint message, ulong wParam, long lParam)
    {
        while (!PostMessageW(window, message, new UIntPtr(wParam), new IntPtr(lParam)))
        {
            int error = Marshal.GetLastWin32Error();
            if (error != 1816) // ERROR_NOT_ENOUGH_QUOTA: the 10,000-message queue is temporarily full.
                throw new InvalidOperationException("PostMessageW failed with error " + error + ".");
            Thread.Sleep(1);
        }
    }

    public static void Key(IntPtr window, int virtualKey)
    {
        Post(window, WM_KEYDOWN, (ulong)virtualKey, 0);
    }

    public static void TypeByte(IntPtr window, byte value)
    {
        const string digits = "0123456789ABCDEF";
        Post(window, WM_CHAR, digits[value >> 4], 0);
        Post(window, WM_CHAR, digits[value & 0x0F], 0);
    }

    public static void TypeBytes(IntPtr window, byte[] values)
    {
        foreach (byte value in values)
            TypeByte(window, value);
    }

    public static void MoveCaret(IntPtr window, int from, int to)
    {
        int current = from;
        while (to - current >= 16)
        {
            Key(window, VK_DOWN);
            current += 16;
        }
        while (current - to >= 16)
        {
            Key(window, VK_UP);
            current -= 16;
        }
        while (current < to)
        {
            Key(window, VK_RIGHT);
            ++current;
        }
        while (current > to)
        {
            Key(window, VK_LEFT);
            --current;
        }
    }

}
'@

function Save-EditorPhase {
    param(
        [IntPtr]$Window,
        [Diagnostics.Process]$Process,
        [string]$Fixture,
        [Collections.Generic.List[byte]]$Expected,
        [string]$Label
    )

    $beforeWrite = [IO.File]::GetLastWriteTimeUtc($Fixture).Ticks
    [BinEditEditorAutomation]::Post($Window, [BinEditEditorAutomation]::WM_COMMAND, 40002, 0)
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    $lastIncompleteObservation = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 50
        if ($Process.HasExited) { throw "BinEdit exited during $Label with code $($Process.ExitCode)." }
        try {
            $info = Get-Item -LiteralPath $Fixture
            if ($info.LastWriteTimeUtc.Ticks -ne $beforeWrite) {
                $bytes = [IO.File]::ReadAllBytes($Fixture)
                if ($bytes.Length -ne $Expected.Count) {
                    # Sparse fixed-size saves retain FILE_SHARE_READ, and an
                    # atomic structural save may be observed immediately before
                    # its replacement is published. Keep polling until the save
                    # barrier exposes the final logical state.
                    $lastIncompleteObservation =
                        "$Label observed $($bytes.Length) bytes during save; expected $($Expected.Count)."
                    continue
                }
                $mismatch = $null
                for ($index = 0; $index -lt $bytes.Length; ++$index) {
                    if ($bytes[$index] -ne $Expected[$index]) {
                        $mismatch = ('{0} observed an in-progress mismatch at 0x{1:X}: actual {2:X2}, expected {3:X2}.' -f
                            $Label, $index, $bytes[$index], $Expected[$index])
                        break
                    }
                }
                if ($mismatch) {
                    $lastIncompleteObservation = $mismatch
                    continue
                }
                return $bytes
            }
        } catch [IO.IOException] {
            # Atomic replacement can briefly hold the path while the save completes.
        }
    }
    if ($lastIncompleteObservation) { throw $lastIncompleteObservation }
    throw "Timed out waiting for the $Label save barrier."
}

function New-RandomOffsets {
    param(
        [Random]$Generator,
        [int]$Count,
        [int]$UpperExclusive
    )

    if ($Count -gt $UpperExclusive) { throw 'Cannot select more unique offsets than the available range.' }
    $unique = [Collections.Generic.HashSet[int]]::new()
    while ($unique.Count -lt $Count) {
        $null = $unique.Add($Generator.Next(0, $UpperExclusive))
    }
    $result = [int[]]$unique
    [Array]::Sort($result)
    return $result
}

$targetSize = $TargetSize
$backspaceCount = $BackspaceCount
$deleteCount = $DeleteCount
$insertCount = $backspaceCount + $deleteCount
$overwriteCount = $OverwriteCount
if ($backspaceCount + $deleteCount -ge $targetSize) {
    throw 'BackspaceCount plus DeleteCount must be smaller than TargetSize.'
}
$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root "x64\$Configuration\BinEdit.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Build output was not found: $executable"
}
if ([BinEditEditorAutomation]::FindWindowW('BinEdit.MainWindow', $null) -ne [IntPtr]::Zero) {
    throw 'Close the existing BinEdit window before running the UI stress test.'
}

$fixture = Join-Path ([IO.Path]::GetTempPath()) "BinEdit.EditorUiStress.$PID.bin"
[IO.File]::WriteAllBytes($fixture, [byte[]]::new(0))
$process = $null
$passed = $false
$stopwatch = [Diagnostics.Stopwatch]::StartNew()

try {
    $argument = '"{0}"' -f $fixture.Replace('"', '\"')
    $process = Start-Process -FilePath $executable -ArgumentList $argument -WindowStyle Hidden -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    $window = [IntPtr]::Zero
    while ($window -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 50
        if ($process.HasExited) { throw "BinEdit exited during startup with code $($process.ExitCode)." }
        $window = [BinEditEditorAutomation]::FindEditorWindow([uint32]$process.Id)
    }
    if ($window -eq [IntPtr]::Zero) { throw 'Timed out waiting for the BinEdit main window.' }
    # The HWND is intentionally visible while asynchronous startup loading shows
    # progress. Wait for the filename title before delivering editor input.
    $fixtureName = [IO.Path]::GetFileName($fixture)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline -and
           -not [BinEditEditorAutomation]::WindowText($window).Contains($fixtureName)) {
        Start-Sleep -Milliseconds 10
        if ($process.HasExited) { throw "BinEdit exited while opening the fixture with code $($process.ExitCode)." }
    }
    if (-not [BinEditEditorAutomation]::WindowText($window).Contains($fixtureName)) {
        throw 'Timed out waiting for the command-line fixture to finish loading.'
    }

    $random = [Random]::new($Seed)
    $initial = [byte[]]::new($targetSize)
    $random.NextBytes($initial)
    $expected = [Collections.Generic.List[byte]]::new($targetSize)
    $expected.AddRange($initial)

    # This is deliberately real hexadecimal-pane input: 40,960 WM_CHAR messages
    # must complete pairs, advance the caret, and materialize exactly 20 KiB.
    [BinEditEditorAutomation]::TypeBytes($window, $initial)
    $caret = $targetSize
    $null = Save-EditorPhase $window $process $fixture $expected 'bulk input'

    # Descending random offsets remain stable as lower-address bytes are removed.
    [int[]]$backspaceOffsets = New-RandomOffsets $random $backspaceCount $expected.Count
    [Array]::Reverse($backspaceOffsets)
    foreach ($offset in $backspaceOffsets) {
        [BinEditEditorAutomation]::MoveCaret($window, $caret, $offset + 1)
        $caret = $offset + 1
        [BinEditEditorAutomation]::Key($window, [BinEditEditorAutomation]::VK_BACK)
        $expected.RemoveAt($offset)
        $caret = $offset
    }
    $null = Save-EditorPhase $window $process $fixture $expected 'random Backspace phase'

    [int[]]$deleteOffsets = New-RandomOffsets $random $deleteCount $expected.Count
    [Array]::Reverse($deleteOffsets)
    foreach ($offset in $deleteOffsets) {
        [BinEditEditorAutomation]::MoveCaret($window, $caret, $offset)
        $caret = $offset
        [BinEditEditorAutomation]::Key($window, [BinEditEditorAutomation]::VK_DELETE)
        $expected.RemoveAt($offset)
    }
    $null = Save-EditorPhase $window $process $fixture $expected 'random Delete phase'

    # The startup default must be insertion mode: each byte is added at a random
    # caret and shifts the tail, restoring the 512 bytes removed above.
    [int[]]$insertionOffsets = New-RandomOffsets $random $insertCount ($expected.Count + 1)
    for ($operation = 0; $operation -lt $insertionOffsets.Length; ++$operation) {
        # Earlier ascending inserts shift every later original position by one.
        $offset = $insertionOffsets[$operation] + $operation
        $value = [byte]$random.Next(0, 256)
        [BinEditEditorAutomation]::MoveCaret($window, $caret, $offset)
        $caret = $offset
        [BinEditEditorAutomation]::TypeByte($window, $value)
        $expected.Insert($offset, $value)
        $caret = $offset + 1
    }
    $null = Save-EditorPhase $window $process $fixture $expected 'random insertion phase'

    if ($expected.Count -ne $targetSize) {
        throw "Oracle final size was $($expected.Count), expected $targetSize."
    }

    # Toggle to overwrite mode, verify that random input preserves file size, and
    # toggle back so the session finishes in its default insertion mode.
    [BinEditEditorAutomation]::Key($window, [BinEditEditorAutomation]::VK_INSERT)
    [int[]]$overwriteOffsets = New-RandomOffsets $random $overwriteCount $expected.Count
    foreach ($offset in $overwriteOffsets) {
        $value = [byte]$random.Next(0, 256)
        [BinEditEditorAutomation]::MoveCaret($window, $caret, $offset)
        $caret = $offset
        [BinEditEditorAutomation]::TypeByte($window, $value)
        $expected[$offset] = $value
        $caret = $offset + 1
    }
    [BinEditEditorAutomation]::Key($window, [BinEditEditorAutomation]::VK_INSERT)

    $actual = Save-EditorPhase $window $process $fixture $expected 'overwrite-mode phase'
    for ($index = 0; $index -lt $targetSize; ++$index) {
        if ($actual[$index] -ne $expected[$index]) {
            throw ('Saved byte mismatch at 0x{0:X}: actual {1:X2}, expected {2:X2}.' -f
                $index, $actual[$index], $expected[$index])
        }
    }

    $passed = $true
    $stopwatch.Stop()
    [pscustomobject]@{
        Result = 'PASS'
        Configuration = $Configuration
        Seed = $Seed
        BulkInputBytes = $initial.Length
        RandomBackspaces = $backspaceCount
        RandomDeletes = $deleteCount
        RandomInserts = $insertCount
        OverwriteModeRewrites = $overwriteCount
        FinalBytes = $actual.Length
        ByteForByteMatch = $true
        ElapsedSeconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
    } | Format-List
}
finally {
    if ($process -and -not $process.HasExited) {
        $window = [BinEditEditorAutomation]::FindEditorWindow([uint32]$process.Id)
        if ($window -ne [IntPtr]::Zero) {
            [BinEditEditorAutomation]::Send($window, [BinEditEditorAutomation]::WM_CLOSE, 0, 0)
            $null = $process.WaitForExit(5000)
        }
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    }
    if ($passed -and (Test-Path -LiteralPath $fixture)) {
        Remove-Item -LiteralPath $fixture -Force
    } elseif (-not $passed) {
        Write-Warning "The failed fixture was retained for diagnosis: $fixture"
    }
}
