param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

# Exercises the application-owned D3D/D2D scrollbar through the real editor
# HWND. A byte written after thumb dragging reveals the exact first visible row,
# allowing the test to verify scrolling without relying on screenshot pixels.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class BinEditScrollBarAutomation
{
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_COMMAND = 0x0111;
    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_CHAR = 0x0102;
    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP = 0x0202;
    public const uint WM_MOUSEMOVE = 0x0200;
    public const uint WM_MOUSEWHEEL = 0x020A;
    public const int VK_INSERT = 0x2D;
    public const int GWL_STYLE = -16;
    public const long WS_VSCROLL = 0x00200000L;
    public const ulong MK_LBUTTON = 0x0001;

    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }

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
    public static extern bool GetClientRect(IntPtr window, out Rect rect);

    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr window);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW", SetLastError = true)]
    public static extern IntPtr GetWindowLongPtrW(IntPtr window, int index);

    [DllImport("user32.dll")]
    public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool SystemParametersInfoW(uint action, uint parameter, out uint value, uint flags);

    public static IntPtr FindEditorWindow(uint processId)
    {
        IntPtr result = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr parameter)
        {
            uint ownerProcess;
            GetWindowThreadProcessId(window, out ownerProcess);
            if (ownerProcess != processId)
                return true;
            StringBuilder className = new StringBuilder(128);
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

    private static IntPtr PointParameter(int x, int y)
    {
        return new IntPtr((y & 0xffff) << 16 | (x & 0xffff));
    }

    public static void Send(IntPtr window, uint message, ulong wParam, long lParam)
    {
        SendMessageW(window, message, new UIntPtr(wParam), new IntPtr(lParam));
    }

    public static void Mouse(IntPtr window, uint message, int x, int y, ulong buttons)
    {
        SendMessageW(window, message, new UIntPtr(buttons), PointParameter(x, y));
    }

    public static void Click(IntPtr window, int x, int y)
    {
        Mouse(window, WM_LBUTTONDOWN, x, y, MK_LBUTTON);
        Mouse(window, WM_LBUTTONUP, x, y, 0);
    }

    public static void TypeByte(IntPtr window, byte value)
    {
        const string digits = "0123456789ABCDEF";
        Send(window, WM_CHAR, digits[value >> 4], 0);
        Send(window, WM_CHAR, digits[value & 0x0f], 0);
    }

    public static void Wheel(IntPtr window, short delta)
    {
        ulong wParam = ((ulong)(ushort)delta) << 16;
        Send(window, WM_MOUSEWHEEL, wParam, 0);
    }

    public static uint WheelScrollLines()
    {
        uint lines;
        return SystemParametersInfoW(0x0068, 0, out lines, 0) ? lines : 3;
    }
}
'@

# Cross-process client coordinates are virtualized unless the automation thread
# uses the same Per-Monitor V2 awareness as BinEdit. -4 is the documented
# DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 pseudo-handle.
$null = [BinEditScrollBarAutomation]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))

function Wait-SavedBytes {
    param(
        [string]$Path,
        [long]$PreviousWriteTicks,
        [hashtable]$ExpectedBytes
    )

    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 50
        try {
            $item = Get-Item -LiteralPath $Path
            if ($item.LastWriteTimeUtc.Ticks -eq $PreviousWriteTicks) { continue }
            $bytes = [IO.File]::ReadAllBytes($Path)
            if ($bytes.Length -ne 64KB) { continue }
            $matches = $true
            foreach ($entry in $ExpectedBytes.GetEnumerator()) {
                if ($bytes[[int]$entry.Key] -ne [byte]$entry.Value) {
                    $matches = $false
                    break
                }
            }
            if ($matches) { return $bytes }
        } catch [IO.IOException] {
            # Retry while the in-place save still owns the file handle.
        }
    }
    throw "Timed out waiting for the expected scrollbar test save. $script:geometryDiagnostic"
}

$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root "x64\$Configuration\BinEdit.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Build output was not found: $executable"
}

$fixture = Join-Path ([IO.Path]::GetTempPath()) "BinEdit.ScrollBarUiTests.$PID.bin"
[IO.File]::WriteAllBytes($fixture, [byte[]]::new(64KB))
$process = $null
$passed = $false

try {
    # Pointer capture is a visible-window contract in USER32. Run normally so
    # the drag exercises the same capture path as a physical mouse gesture.
    $process = Start-Process -FilePath $executable -ArgumentList ('"{0}"' -f $fixture) -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    $window = [IntPtr]::Zero
    while ($window -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 50
        if ($process.HasExited) { throw "BinEdit exited during startup with code $($process.ExitCode)." }
        $window = [BinEditScrollBarAutomation]::FindEditorWindow([uint32]$process.Id)
    }
    if ($window -eq [IntPtr]::Zero) { throw 'Timed out waiting for the BinEdit main window.' }

    # HWND creation precedes command-line file loading. Wait for the title update
    # so pointer input cannot race the initial empty-document scrollbar model.
    $fixtureName = [IO.Path]::GetFileName($fixture)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline -and
           -not [BinEditScrollBarAutomation]::WindowText($window).Contains($fixtureName)) {
        Start-Sleep -Milliseconds 50
        if ($process.HasExited) { throw "BinEdit exited while opening the fixture with code $($process.ExitCode)." }
    }
    if (-not [BinEditScrollBarAutomation]::WindowText($window).Contains($fixtureName)) {
        throw 'Timed out waiting for the command-line fixture to become active.'
    }

    $style = [BinEditScrollBarAutomation]::GetWindowLongPtrW(
        $window, [BinEditScrollBarAutomation]::GWL_STYLE).ToInt64()
    if (($style -band [BinEditScrollBarAutomation]::WS_VSCROLL) -ne 0) {
        throw 'The editor still has the native WS_VSCROLL style.'
    }

    $client = [BinEditScrollBarAutomation+Rect]::new()
    if (-not [BinEditScrollBarAutomation]::GetClientRect($window, [ref]$client)) {
        throw 'GetClientRect failed.'
    }
    $dpi = [BinEditScrollBarAutomation]::GetDpiForWindow($window)
    $scale = $dpi / 96.0
    $header = [Math]::Round(44 * $scale) + [Math]::Round(48 * $scale)
    $status = [Math]::Round(36 * $scale)
    $rowHeight = [Math]::Round(24 * $scale)
    $visibleRows = [int][Math]::Floor(($client.Bottom - $header - $status) / $rowHeight)
    if ($visibleRows -lt 1) { throw 'The test window has no complete data rows.' }

    $totalRows = [int](64KB / 16 + 1)
    $maximumFirst = $totalRows - $visibleRows
    $trackTop = $header + [Math]::Round(4 * $scale)
    $trackBottom = $client.Bottom - $status - [Math]::Round(4 * $scale)
    $trackHeight = $trackBottom - $trackTop
    $thumbHeight = [Math]::Max([Math]::Round(28 * $scale), $trackHeight * $visibleRows / $totalRows)
    $thumbHeight = [Math]::Min($trackHeight, $thumbHeight)
    $scrollX = [int]($client.Right - [Math]::Round(8 * $scale) - [Math]::Round(10 * $scale))
    $initialThumbCenter = [int]($trackTop + $thumbHeight / 2)
    # Deliberately overshoot the final thumb center; production code clamps the
    # proposed top, avoiding half-pixel ambiguity at fractional monitor DPI.
    $dragEndY = [int]$trackBottom
    $script:geometryDiagnostic = (("Client={0}x{1} DPI={2} Header={3} Status={4} Row={5} Visible={6} " +
        "Track={7}..{8} Thumb={9} X={10}") -f $client.Right, $client.Bottom, $dpi,
        $header, $status, $rowHeight, $visibleRows, $trackTop, $trackBottom,
        $thumbHeight, $scrollX)

    # Drag the custom thumb to its final legal position.
    [BinEditScrollBarAutomation]::Mouse($window,
        [BinEditScrollBarAutomation]::WM_LBUTTONDOWN, $scrollX, $initialThumbCenter,
        [BinEditScrollBarAutomation]::MK_LBUTTON)
    [BinEditScrollBarAutomation]::Mouse($window,
        [BinEditScrollBarAutomation]::WM_MOUSEMOVE, $scrollX, $dragEndY,
        [BinEditScrollBarAutomation]::MK_LBUTTON)
    [BinEditScrollBarAutomation]::Mouse($window,
        [BinEditScrollBarAutomation]::WM_LBUTTONUP, $scrollX, $dragEndY, 0)

    # Overwrite the first visible byte. Its durable address proves the mapped row.
    [BinEditScrollBarAutomation]::Send($window,
        [BinEditScrollBarAutomation]::WM_KEYDOWN, [BinEditScrollBarAutomation]::VK_INSERT, 0)
    $effectiveWidth = $client.Right / $scale
    $hexX = [Math]::Round($(if ($effectiveWidth -lt 900) { 104 } else { 124 }) * $scale)
    $dataX = [int]($hexX + 2)
    $dataY = [int]($header + $rowHeight / 2)
    [BinEditScrollBarAutomation]::Click($window, $dataX, $dataY)
    [BinEditScrollBarAutomation]::TypeByte($window, 0xA5)
    $bottomOffset = $maximumFirst * 16
    $beforeSave = (Get-Item -LiteralPath $fixture).LastWriteTimeUtc.Ticks
    [BinEditScrollBarAutomation]::Send($window, [BinEditScrollBarAutomation]::WM_COMMAND, 40002, 0)
    $actual = Wait-SavedBytes $fixture $beforeSave @{$bottomOffset = 0xA5}

    # Clicking immediately above the bottom-positioned thumb pages upward once.
    $bottomThumbTop = $trackBottom - $thumbHeight
    [BinEditScrollBarAutomation]::Click($window, $scrollX, [int]($bottomThumbTop - 2))
    [BinEditScrollBarAutomation]::Click($window, $dataX, $dataY)
    [BinEditScrollBarAutomation]::TypeByte($window, 0x5A)
    $pagedFirst = [Math]::Max(0, $maximumFirst - $visibleRows)
    $pagedOffset = $pagedFirst * 16
    $beforeSave = (Get-Item -LiteralPath $fixture).LastWriteTimeUtc.Ticks
    [BinEditScrollBarAutomation]::Send($window, [BinEditScrollBarAutomation]::WM_COMMAND, 40002, 0)
    $actual = Wait-SavedBytes $fixture $beforeSave @{$bottomOffset = 0xA5; $pagedOffset = 0x5A}

    # Two half-notch messages validate high-resolution wheel accumulation. The
    # expected distance honors the current user's Windows wheel setting.
    $wheelLines = [BinEditScrollBarAutomation]::WheelScrollLines()
    $wheelFirst = $pagedFirst
    $expected = @{$bottomOffset = 0xA5; $pagedOffset = 0x5A}
    if ($wheelLines -ne 0) {
        [BinEditScrollBarAutomation]::Wheel($window, 60)
        [BinEditScrollBarAutomation]::Wheel($window, 60)
        $wheelDistance = if ($wheelLines -eq [uint32]::MaxValue) {
            $visibleRows
        } else {
            [int]$wheelLines
        }
        $wheelFirst = [Math]::Max(0, $pagedFirst - $wheelDistance)
        $wheelOffset = $wheelFirst * 16
        [BinEditScrollBarAutomation]::Click($window, $dataX, $dataY)
        [BinEditScrollBarAutomation]::TypeByte($window, 0xC3)
        $expected[$wheelOffset] = 0xC3
        $beforeSave = (Get-Item -LiteralPath $fixture).LastWriteTimeUtc.Ticks
        [BinEditScrollBarAutomation]::Send($window, [BinEditScrollBarAutomation]::WM_COMMAND, 40002, 0)
        $actual = Wait-SavedBytes $fixture $beforeSave $expected
    }

    $nonZero = for ($index = 0; $index -lt $actual.Length; ++$index) {
        if ($actual[$index] -ne 0) { $index }
    }
    [int[]]$expectedOffsets = $expected.Keys
    [Array]::Sort($expectedOffsets)
    if ($nonZero.Count -ne $expectedOffsets.Count) {
        throw "Unexpected changed offsets: $($nonZero -join ', ')."
    }
    for ($index = 0; $index -lt $expectedOffsets.Count; ++$index) {
        if ($nonZero[$index] -ne $expectedOffsets[$index]) {
            throw "Unexpected changed offsets: $($nonZero -join ', ')."
        }
    }

    $passed = $true
    [pscustomobject]@{
        Result = 'PASS'
        Configuration = $Configuration
        NativeScrollStyle = $false
        Dpi = $dpi
        CompleteRows = $visibleRows
        DraggedFirstRow = $maximumFirst
        TrackPageFirstRow = $pagedFirst
        WheelFirstRow = $wheelFirst
        ByteForByteMatch = $true
    } | Format-List
}
finally {
    if ($process -and -not $process.HasExited) {
        $window = [BinEditScrollBarAutomation]::FindEditorWindow([uint32]$process.Id)
        if ($window -ne [IntPtr]::Zero) {
            [BinEditScrollBarAutomation]::Send($window, [BinEditScrollBarAutomation]::WM_CLOSE, 0, 0)
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
