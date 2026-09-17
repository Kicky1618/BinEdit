param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

# Opens enough real files to overflow the D2D tab strip, scrolls its independent
# horizontal viewport with Shift+wheel, selects the newly revealed first tab,
# and proves the visual/hit-test mapping by saving a byte into that exact file.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class BinEditTabStripAutomation
{
    public const string MainWindowClass = "BinEdit.MainWindow";
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_COMMAND = 0x0111;
    public const uint WM_CHAR = 0x0102;
    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP = 0x0202;
    public const uint WM_MOUSEWHEEL = 0x020A;
    private const ulong MK_SHIFT = 0x0004;

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }

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
    public static extern bool GetClientRect(IntPtr window, out RECT rect);

    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr window);

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

    public static string WindowText(IntPtr window)
    {
        StringBuilder text = new StringBuilder(1024);
        GetWindowTextW(window, text, text.Capacity);
        return text.ToString();
    }

    private static void SendPoint(IntPtr window, uint message, ulong wParam, int x, int y)
    {
        long packed = ((long)(ushort)(short)y << 16) | (ushort)(short)x;
        SendMessageW(window, message, new UIntPtr(wParam), new IntPtr(packed));
    }

    public static void ShiftWheelLeft(IntPtr window)
    {
        ulong wParam = ((ulong)(ushort)(short)120 << 16) | MK_SHIFT;
        SendMessageW(window, WM_MOUSEWHEEL, new UIntPtr(wParam), IntPtr.Zero);
    }

    public static void Click(IntPtr window, int x, int y)
    {
        SendPoint(window, WM_LBUTTONDOWN, 1, x, y);
        SendPoint(window, WM_LBUTTONUP, 0, x, y);
    }

    public static void TypeByte(IntPtr window, byte value)
    {
        const string digits = "0123456789ABCDEF";
        SendMessageW(window, WM_CHAR, new UIntPtr(digits[value >> 4]), IntPtr.Zero);
        SendMessageW(window, WM_CHAR, new UIntPtr(digits[value & 15]), IntPtr.Zero);
    }

    public static void Command(IntPtr window, int command)
    {
        SendMessageW(window, WM_COMMAND, new UIntPtr((uint)command), IntPtr.Zero);
    }

    public static void Close(IntPtr window)
    {
        SendMessageW(window, WM_CLOSE, UIntPtr.Zero, IntPtr.Zero);
    }
}
'@

$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root "x64\$Configuration\BinEdit.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Build output was not found: $executable"
}

$tabCount = 28
$fixtures = [Collections.Generic.List[string]]::new()
$process = $null
$passed = $false
try {
    for ($index = 0; $index -lt $tabCount; ++$index) {
        $path = Join-Path ([IO.Path]::GetTempPath()) ('BinEdit.TabStrip.{0}.{1:D2}.bin' -f $PID, $index)
        [IO.File]::WriteAllBytes($path, [byte[]]@([byte]$index))
        $fixtures.Add($path)
    }
    $arguments = @($fixtures | ForEach-Object { '"{0}"' -f $_ })
    $process = Start-Process -FilePath $executable -ArgumentList $arguments -WindowStyle Hidden -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    [IntPtr]$window = [IntPtr]::Zero
    do {
        if ($process.HasExited) { throw "BinEdit exited during startup with code $($process.ExitCode)." }
        $window = [BinEditTabStripAutomation]::FindMainWindow([uint32]$process.Id)
        if ($window -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($window -eq [IntPtr]::Zero) { throw 'Timed out waiting for the main window.' }
    $lastFileName = [IO.Path]::GetFileName($fixtures[$tabCount - 1])
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $deadline -and
           -not [BinEditTabStripAutomation]::WindowText($window).Contains($lastFileName)) {
        Start-Sleep -Milliseconds 10
    }
    if (-not [BinEditTabStripAutomation]::WindowText($window).Contains($lastFileName)) {
        throw 'Timed out waiting for every command-line tab to finish loading.'
    }

    # Startup activates the final tab and therefore scrolls the viewport to its
    # right edge. Repeated positive Shift+wheel notches must reveal tab zero.
    for ($notch = 0; $notch -lt $tabCount; ++$notch) {
        [BinEditTabStripAutomation]::ShiftWheelLeft($window)
    }

    $rect = [BinEditTabStripAutomation+RECT]::new()
    if (-not [BinEditTabStripAutomation]::GetClientRect($window, [ref]$rect)) {
        throw 'GetClientRect failed.'
    }
    $scale = [BinEditTabStripAutomation]::GetDpiForWindow($window) / 96.0
    $margin = [Math]::Round(8.0 * $scale)
    $available = ($rect.Right - $rect.Left) - $margin * 2
    $tabWidth = [Math]::Max([Math]::Round(56.0 * $scale),
        [Math]::Min([Math]::Round(220.0 * $scale), $available / $tabCount))
    $tabX = [int]($margin + [Math]::Min($tabWidth * 0.3, 30.0 * $scale))
    $tabY = [int][Math]::Round(20.0 * $scale)
    [BinEditTabStripAutomation]::Click($window, $tabX, $tabY)
    [BinEditTabStripAutomation]::TypeByte($window, 0xFE)
    [BinEditTabStripAutomation]::Command($window, 40002)

    [byte[]]$first = [IO.File]::ReadAllBytes($fixtures[0])
    if ($first.Length -ne 2 -or $first[0] -ne 0xFE -or $first[1] -ne 0x00) {
        throw 'Shift+wheel did not reveal and select the first tab.'
    }
    [byte[]]$last = [IO.File]::ReadAllBytes($fixtures[$tabCount - 1])
    if ($last.Length -ne 1 -or $last[0] -ne [byte]($tabCount - 1)) {
        throw 'Horizontal tab scrolling modified the previously active final tab.'
    }

    [BinEditTabStripAutomation]::Close($window)
    if (-not $process.WaitForExit(5000) -or $process.ExitCode -ne 0) {
        throw 'BinEdit did not exit cleanly after the tab-strip test.'
    }
    $passed = $true
    [pscustomobject]@{
        Result = 'PASS'
        Configuration = $Configuration
        OpenTabs = $tabCount
        ShiftWheelHorizontalScroll = $true
        RevealedFirstTabHitTest = $true
        ByteForByteTargeting = $true
    } | Format-List
}
finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    if ($passed) {
        foreach ($path in $fixtures) {
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
        }
    } else {
        Write-Warning "Failed tab-strip fixtures retained: $($fixtures -join ' ; ')"
    }
}
