param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

# Verifies middle-button auto-scroll through the real top-level editor HWND. The
# durable offsets written after downward and upward acceleration reveal the
# viewport position without coupling the test to rendered pixels.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class BinEditMiddleScrollAutomation
{
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_COMMAND = 0x0111;
    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_CHAR = 0x0102;
    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP = 0x0202;
    public const uint WM_MBUTTONDOWN = 0x0207;
    public const uint WM_MBUTTONUP = 0x0208;
    public const uint WM_MOUSEMOVE = 0x0200;
    public const int VK_INSERT = 0x2D;
    public const ulong MK_LBUTTON = 0x0001;
    public const ulong MK_MBUTTON = 0x0010;
    public const uint MOUSEEVENTF_MIDDLEDOWN = 0x0020;
    public const uint MOUSEEVENTF_MIDDLEUP = 0x0040;
    public const int IDC_PAN_MIDDLE = 32654;
    public const int IDC_PAN_NORTH = 32655;
    public const int IDC_PAN_SOUTH = 32657;

    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    public struct Point { public int X, Y; }

    [StructLayout(LayoutKind.Sequential)]
    public struct CursorInfo
    {
        public int Size;
        public int Flags;
        public IntPtr Cursor;
        public Point ScreenPosition;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IconInfo
    {
        [MarshalAs(UnmanagedType.Bool)] public bool IsIcon;
        public uint HotspotX;
        public uint HotspotY;
        public IntPtr MaskBitmap;
        public IntPtr ColorBitmap;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct BitmapInfo
    {
        public int Type;
        public int Width;
        public int Height;
        public int WidthBytes;
        public ushort Planes;
        public ushort BitsPixel;
        public IntPtr Bits;
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
    public static extern bool GetClientRect(IntPtr window, out Rect rect);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool ClientToScreen(IntPtr window, ref Point point);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr window);

    [DllImport("user32.dll")]
    private static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extraInfo);

    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr window);

    [DllImport("user32.dll")]
    public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);

    [DllImport("user32.dll", EntryPoint = "LoadCursorW", SetLastError = true)]
    private static extern IntPtr LoadCursor(IntPtr instance, IntPtr resource);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetCursorInfo(ref CursorInfo info);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetIconInfo(IntPtr icon, out IconInfo info);

    [DllImport("gdi32.dll", EntryPoint = "GetObjectW", SetLastError = true)]
    private static extern int GetObject(IntPtr value, int size, out BitmapInfo info);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern int GetBitmapBits(IntPtr bitmap, int count, byte[] bits);

    [DllImport("gdi32.dll")]
    private static extern bool DeleteObject(IntPtr value);

    public static IntPtr SystemCursor(int resource)
    {
        return LoadCursor(IntPtr.Zero, new IntPtr(resource));
    }

    public static IntPtr CurrentCursor()
    {
        CursorInfo info = new CursorInfo();
        info.Size = Marshal.SizeOf(typeof(CursorInfo));
        if (!GetCursorInfo(ref info))
            throw new InvalidOperationException("GetCursorInfo failed.");
        return info.Cursor;
    }

    public static Point CurrentCursorPosition()
    {
        CursorInfo info = new CursorInfo();
        info.Size = Marshal.SizeOf(typeof(CursorInfo));
        if (!GetCursorInfo(ref info))
            throw new InvalidOperationException("GetCursorInfo failed.");
        return info.ScreenPosition;
    }

    private static string BitmapFingerprint(IntPtr bitmap)
    {
        if (bitmap == IntPtr.Zero)
            return "none";
        BitmapInfo info;
        if (GetObject(bitmap, Marshal.SizeOf(typeof(BitmapInfo)), out info) == 0)
            throw new InvalidOperationException("GetObject failed for a cursor bitmap.");
        int byteCount = checked(Math.Abs(info.Height) * info.WidthBytes);
        byte[] bits = new byte[byteCount];
        if (GetBitmapBits(bitmap, byteCount, bits) != byteCount)
            throw new InvalidOperationException("GetBitmapBits failed for a cursor bitmap.");
        ulong hash = 14695981039346656037UL;
        foreach (byte value in bits)
        {
            hash ^= value;
            hash *= 1099511628211UL;
        }
        return info.Width + "x" + info.Height + ":" + info.Planes + ":" +
            info.BitsPixel + ":" + hash.ToString("X16");
    }

    private static string CursorFingerprint(IntPtr cursor)
    {
        IconInfo info;
        if (!GetIconInfo(cursor, out info))
            throw new InvalidOperationException("GetIconInfo failed for a cursor.");
        try
        {
            return info.HotspotX + ":" + info.HotspotY + ":" +
                BitmapFingerprint(info.MaskBitmap) + ":" + BitmapFingerprint(info.ColorBitmap);
        }
        finally
        {
            if (info.MaskBitmap != IntPtr.Zero) DeleteObject(info.MaskBitmap);
            if (info.ColorBitmap != IntPtr.Zero) DeleteObject(info.ColorBitmap);
        }
    }

    public static bool CurrentCursorMatchesSystemCursor(int resource)
    {
        return CursorFingerprint(CurrentCursor()) == CursorFingerprint(SystemCursor(resource));
    }

    public static string CurrentCursorDescription()
    {
        string actual = CursorFingerprint(CurrentCursor());
        return actual + " [middle=" + CursorFingerprint(SystemCursor(IDC_PAN_MIDDLE)) +
            ", north=" + CursorFingerprint(SystemCursor(IDC_PAN_NORTH)) +
            ", south=" + CursorFingerprint(SystemCursor(IDC_PAN_SOUTH)) + "]";
    }

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

    public static void MiddleClick(IntPtr window, int x, int y)
    {
        Mouse(window, WM_MBUTTONDOWN, x, y, MK_MBUTTON);
        Mouse(window, WM_MBUTTONUP, x, y, 0);
    }

    public static void PhysicalMiddleClick()
    {
        mouse_event(MOUSEEVENTF_MIDDLEDOWN, 0, 0, 0, UIntPtr.Zero);
        mouse_event(MOUSEEVENTF_MIDDLEUP, 0, 0, 0, UIntPtr.Zero);
    }

    public static void LeftClick(IntPtr window, int x, int y)
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
}
'@

$null = [BinEditMiddleScrollAutomation]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))

function Wait-Save {
    param(
        [string]$Path,
        [long]$PreviousWriteTicks,
        [int]$ExpectedNonZeroCount
    )

    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 50
        try {
            $item = Get-Item -LiteralPath $Path
            if ($item.LastWriteTimeUtc.Ticks -eq $PreviousWriteTicks) { continue }
            $bytes = [IO.File]::ReadAllBytes($Path)
            if ($bytes.Length -ne 64KB) { continue }
            [int[]]$changed = @(for ($index = 0; $index -lt $bytes.Length; ++$index) {
                if ($bytes[$index] -ne 0) { $index }
            })
            if ($changed.Count -eq $ExpectedNonZeroCount) {
                return [pscustomobject]@{ Bytes = $bytes; Changed = [int[]]$changed }
            }
        } catch [IO.IOException] {
            # Retry until the in-place write and flush transaction has completed.
        }
    }
    throw 'Timed out waiting for the middle-scroll test save.'
}

$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root "x64\$Configuration\BinEdit.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Build output was not found: $executable"
}

$fixture = Join-Path ([IO.Path]::GetTempPath()) "BinEdit.MiddleScrollUiTests.$PID.bin"
[IO.File]::WriteAllBytes($fixture, [byte[]]::new(64KB))
$process = $null
$passed = $false
$originalCursorPosition = [BinEditMiddleScrollAutomation]::CurrentCursorPosition()

try {
    # A visible top-level window is required because USER32 capture intentionally
    # keeps delivering mouse movement after the middle button is released.
    $process = Start-Process -FilePath $executable -ArgumentList ('"{0}"' -f $fixture) -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    $window = [IntPtr]::Zero
    while ($window -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 50
        if ($process.HasExited) { throw "BinEdit exited during startup with code $($process.ExitCode)." }
        $window = [BinEditMiddleScrollAutomation]::FindEditorWindow([uint32]$process.Id)
    }
    if ($window -eq [IntPtr]::Zero) { throw 'Timed out waiting for the BinEdit main window.' }

    $fixtureName = [IO.Path]::GetFileName($fixture)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline -and
           -not [BinEditMiddleScrollAutomation]::WindowText($window).Contains($fixtureName)) {
        Start-Sleep -Milliseconds 50
    }
    if (-not [BinEditMiddleScrollAutomation]::WindowText($window).Contains($fixtureName)) {
        throw 'Timed out waiting for the command-line fixture to become active.'
    }

    $client = [BinEditMiddleScrollAutomation+Rect]::new()
    if (-not [BinEditMiddleScrollAutomation]::GetClientRect($window, [ref]$client)) {
        throw 'GetClientRect failed.'
    }
    $dpi = [BinEditMiddleScrollAutomation]::GetDpiForWindow($window)
    $scale = $dpi / 96.0
    $header = [Math]::Round(44 * $scale) + [Math]::Round(48 * $scale)
    $status = [Math]::Round(36 * $scale)
    $rowHeight = [Math]::Round(24 * $scale)
    $visibleRows = [int][Math]::Floor(($client.Bottom - $header - $status) / $rowHeight)
    if ($visibleRows -lt 1) { throw 'The test window has no complete data rows.' }
    $effectiveWidth = $client.Right / $scale
    $hexX = [Math]::Round($(if ($effectiveWidth -lt 900) { 104 } else { 124 }) * $scale)
    $dataX = [int]($hexX + 2)
    $firstRowY = [int]($header + $rowHeight / 2)
    $anchorX = [int]($hexX + $rowHeight)
    $anchorY = [int]($header + $visibleRows * $rowHeight / 2)

    # SetCursor affects the visible cursor only while the pointer belongs to the
    # calling window or that window owns capture. Position the real pointer over
    # the editor so this test observes USER32's actual cursor arbitration rather
    # than only the synthetic messages used for deterministic scroll distances.
    $anchorScreen = [BinEditMiddleScrollAutomation+Point]::new()
    $anchorScreen.X = $anchorX
    $anchorScreen.Y = $anchorY
    if (-not [BinEditMiddleScrollAutomation]::ClientToScreen($window, [ref]$anchorScreen)) {
        throw 'ClientToScreen failed for the auto-scroll anchor.'
    }
    $null = [BinEditMiddleScrollAutomation]::SetForegroundWindow($window)
    if (-not [BinEditMiddleScrollAutomation]::SetCursorPos($anchorScreen.X, $anchorScreen.Y)) {
        throw 'SetCursorPos failed for the auto-scroll anchor.'
    }
    Start-Sleep -Milliseconds 50

    # Accelerate toward the bottom, then stop with the second middle click.
    [BinEditMiddleScrollAutomation]::PhysicalMiddleClick()
    if (-not [BinEditMiddleScrollAutomation]::CurrentCursorMatchesSystemCursor(
            [BinEditMiddleScrollAutomation]::IDC_PAN_MIDDLE)) {
        throw 'Auto-scroll did not install the stationary pan cursor inside its dead zone.'
    }
    [BinEditMiddleScrollAutomation]::Mouse($window,
        [BinEditMiddleScrollAutomation]::WM_MOUSEMOVE, $anchorX, $client.Bottom + 600, 0)
    Start-Sleep -Milliseconds 20
    if (-not [BinEditMiddleScrollAutomation]::CurrentCursorMatchesSystemCursor(
            [BinEditMiddleScrollAutomation]::IDC_PAN_SOUTH)) {
        throw "Downward auto-scroll did not install the south pan cursor: $([BinEditMiddleScrollAutomation]::CurrentCursorDescription())"
    }
    Start-Sleep -Milliseconds 600
    [BinEditMiddleScrollAutomation]::PhysicalMiddleClick()

    [BinEditMiddleScrollAutomation]::Send($window,
        [BinEditMiddleScrollAutomation]::WM_KEYDOWN, [BinEditMiddleScrollAutomation]::VK_INSERT, 0)
    [BinEditMiddleScrollAutomation]::LeftClick($window, $dataX, $firstRowY)
    [BinEditMiddleScrollAutomation]::TypeByte($window, 0xA7)
    $beforeSave = (Get-Item -LiteralPath $fixture).LastWriteTimeUtc.Ticks
    [BinEditMiddleScrollAutomation]::Send($window, [BinEditMiddleScrollAutomation]::WM_COMMAND, 40002, 0)
    $downResult = Wait-Save $fixture $beforeSave 1
    $downOffset = $downResult.Changed[0]
    if ($downResult.Bytes[$downOffset] -ne 0xA7 -or $downOffset -lt 48KB -or ($downOffset % 16) -ne 0) {
        throw "Downward auto-scroll stopped at unexpected offset $downOffset."
    }

    # Reverse with the pointer above the window and verify that the viewport can
    # traverse back toward the beginning at the same accelerated rate.
    $null = [BinEditMiddleScrollAutomation]::SetCursorPos($anchorScreen.X, $anchorScreen.Y)
    Start-Sleep -Milliseconds 50
    [BinEditMiddleScrollAutomation]::PhysicalMiddleClick()
    if (-not [BinEditMiddleScrollAutomation]::CurrentCursorMatchesSystemCursor(
            [BinEditMiddleScrollAutomation]::IDC_PAN_MIDDLE)) {
        throw 'The second auto-scroll operation did not restore the stationary pan cursor.'
    }
    [BinEditMiddleScrollAutomation]::Mouse($window,
        [BinEditMiddleScrollAutomation]::WM_MOUSEMOVE, $anchorX, -600, 0)
    Start-Sleep -Milliseconds 20
    if (-not [BinEditMiddleScrollAutomation]::CurrentCursorMatchesSystemCursor(
            [BinEditMiddleScrollAutomation]::IDC_PAN_NORTH)) {
        throw 'Upward auto-scroll did not install the north pan cursor.'
    }
    Start-Sleep -Milliseconds 600
    [BinEditMiddleScrollAutomation]::PhysicalMiddleClick()
    [BinEditMiddleScrollAutomation]::LeftClick($window, $dataX, $firstRowY)
    [BinEditMiddleScrollAutomation]::TypeByte($window, 0x5C)
    $beforeSave = (Get-Item -LiteralPath $fixture).LastWriteTimeUtc.Ticks
    [BinEditMiddleScrollAutomation]::Send($window, [BinEditMiddleScrollAutomation]::WM_COMMAND, 40002, 0)
    $upResult = Wait-Save $fixture $beforeSave 2
    $upOffset = ($upResult.Changed | Where-Object { $_ -ne $downOffset })[0]
    if ($upResult.Bytes[$upOffset] -ne 0x5C -or $upOffset -ge 16KB -or ($upOffset % 16) -ne 0) {
        throw "Upward auto-scroll stopped at unexpected offset $upOffset."
    }

    $passed = $true
    [pscustomobject]@{
        Result = 'PASS'
        Configuration = $Configuration
        Dpi = $dpi
        CompleteRows = $visibleRows
        DownwardOffset = $downOffset
        UpwardOffset = $upOffset
        AcceleratedBothDirections = $true
        DirectionalCursors = $true
        ByteForByteMatch = $true
    } | Format-List
}
finally {
    $null = [BinEditMiddleScrollAutomation]::SetCursorPos(
        $originalCursorPosition.X, $originalCursorPosition.Y)
    if ($process -and -not $process.HasExited) {
        $window = [BinEditMiddleScrollAutomation]::FindEditorWindow([uint32]$process.Id)
        if ($window -ne [IntPtr]::Zero) {
            [BinEditMiddleScrollAutomation]::Send($window, [BinEditMiddleScrollAutomation]::WM_CLOSE, 0, 0)
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
