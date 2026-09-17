param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

# Opens a large file through the production single-instance route and verifies
# that its address range is file-mapped instead of copied into private committed
# RAM. Closing the final named tab must remove that mapped range and return the
# permanent main window to a pristine Untitled session.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class BinEditMemoryAutomation
{
    public const string MainWindowClass = "BinEdit.MainWindow";
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_COMMAND = 0x0111;
    public const uint WM_INITMENUPOPUP = 0x0117;
    private const uint MF_BYCOMMAND = 0x00000000;
    private const uint MF_GRAYED = 0x00000001;
    private const uint MF_DISABLED = 0x00000002;
    private const uint PROCESS_QUERY_INFORMATION = 0x0400;
    private const uint PROCESS_VM_READ = 0x0010;
    private const uint MEM_COMMIT = 0x1000;
    private const uint MEM_MAPPED = 0x40000;

    private delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

    [StructLayout(LayoutKind.Sequential)]
    private struct MEMORY_BASIC_INFORMATION
    {
        public IntPtr BaseAddress;
        public IntPtr AllocationBase;
        public uint AllocationProtect;
        public UIntPtr RegionSize;
        public uint State;
        public uint Protect;
        public uint Type;
    }

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
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inheritHandle, uint processId);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern UIntPtr VirtualQueryEx(IntPtr process, IntPtr address,
        out MEMORY_BASIC_INFORMATION information, UIntPtr informationLength);
    [DllImport("psapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetMappedFileNameW(IntPtr process, IntPtr address,
        StringBuilder fileName, uint size);

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

    public static bool CloseTabEnabled(IntPtr window)
    {
        IntPtr menu = GetMenu(window);
        IntPtr fileMenu = GetSubMenu(menu, 0);
        if (menu == IntPtr.Zero || fileMenu == IntPtr.Zero)
            throw new InvalidOperationException("Could not resolve the File menu.");
        SendMessageW(window, WM_INITMENUPOPUP,
            new UIntPtr(unchecked((ulong)fileMenu.ToInt64())), IntPtr.Zero);
        uint state = GetMenuState(fileMenu, 40005, MF_BYCOMMAND);
        if (state == UInt32.MaxValue)
            throw new InvalidOperationException("Could not resolve Close tab.");
        return (state & (MF_GRAYED | MF_DISABLED)) == 0;
    }

    public static void CloseTab(IntPtr window)
    {
        SendMessageW(window, WM_COMMAND, new UIntPtr(40005), IntPtr.Zero);
    }

    public static void CloseWindow(IntPtr window)
    {
        SendMessageW(window, WM_CLOSE, UIntPtr.Zero, IntPtr.Zero);
    }

    public static ulong MappedCommittedBytes(uint processId)
    {
        IntPtr process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
            false, processId);
        if (process == IntPtr.Zero)
            throw new InvalidOperationException("OpenProcess failed for VirtualQueryEx sampling.");
        try
        {
            ulong total = 0;
            ulong address = 0;
            ulong maximumAddress = IntPtr.Size == 8 ? 0x00007FFFFFFF0000UL : 0xFFF00000UL;
            UIntPtr structureSize = new UIntPtr((uint)Marshal.SizeOf(typeof(MEMORY_BASIC_INFORMATION)));
            while (address < maximumAddress)
            {
                MEMORY_BASIC_INFORMATION information;
                if (VirtualQueryEx(process, new IntPtr(unchecked((long)address)),
                    out information, structureSize) == UIntPtr.Zero) break;
                ulong baseAddress = unchecked((ulong)information.BaseAddress.ToInt64());
                ulong regionSize = information.RegionSize.ToUInt64();
                if (regionSize == 0) break;
                if (information.State == MEM_COMMIT && information.Type == MEM_MAPPED)
                    total += regionSize;
                ulong next = baseAddress + regionSize;
                if (next <= address) break;
                address = next;
            }
            return total;
        }
        finally { CloseHandle(process); }
    }

    public static ulong MappedFileBytes(uint processId, string leafName)
    {
        IntPtr process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
            false, processId);
        if (process == IntPtr.Zero)
            throw new InvalidOperationException("OpenProcess failed for mapped-file sampling.");
        try
        {
            ulong total = 0;
            ulong address = 0;
            ulong maximumAddress = IntPtr.Size == 8 ? 0x00007FFFFFFF0000UL : 0xFFF00000UL;
            UIntPtr structureSize = new UIntPtr((uint)Marshal.SizeOf(typeof(MEMORY_BASIC_INFORMATION)));
            string suffix = "\\" + leafName;
            while (address < maximumAddress)
            {
                MEMORY_BASIC_INFORMATION information;
                if (VirtualQueryEx(process, new IntPtr(unchecked((long)address)),
                    out information, structureSize) == UIntPtr.Zero) break;
                ulong baseAddress = unchecked((ulong)information.BaseAddress.ToInt64());
                ulong regionSize = information.RegionSize.ToUInt64();
                if (regionSize == 0) break;
                if (information.State == MEM_COMMIT && information.Type == MEM_MAPPED)
                {
                    StringBuilder mappedName = new StringBuilder(32768);
                    if (GetMappedFileNameW(process, information.BaseAddress, mappedName,
                        (uint)mappedName.Capacity) > 0 &&
                        mappedName.ToString().EndsWith(suffix, StringComparison.OrdinalIgnoreCase))
                        total += regionSize;
                }
                ulong next = baseAddress + regionSize;
                if (next <= address) break;
                address = next;
            }
            return total;
        }
        finally { CloseHandle(process); }
    }
}
'@

function Wait-MainWindow {
    param([Diagnostics.Process]$Process)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        if ($Process.HasExited) { throw "BinEdit exited during startup with code $($Process.ExitCode)." }
        [IntPtr]$window = [BinEditMemoryAutomation]::FindMainWindow([uint32]$Process.Id)
        if ($window -ne [IntPtr]::Zero) { return $window }
        Start-Sleep -Milliseconds 20
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Timed out waiting for the main window.'
}

function Get-MemorySample {
    param([Diagnostics.Process]$Process)
    $Process.Refresh()
    [pscustomobject]@{
        PrivateBytes = [long]$Process.PrivateMemorySize64
        WorkingSetBytes = [long]$Process.WorkingSet64
        MappedCommittedBytes = [long][BinEditMemoryAutomation]::MappedCommittedBytes([uint32]$Process.Id)
    }
}

$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root "x64\$Configuration\BinEdit.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Build output was not found: $executable"
}

$fixtureSize = 96L * 1024L * 1024L
$minimumMappedGrowth = 88L * 1024L * 1024L
$maximumPrivateGrowth = 48L * 1024L * 1024L
$fixture = Join-Path ([IO.Path]::GetTempPath()) "BinEdit.MemoryRelease.$PID.bin"
$stream = [IO.File]::Open($fixture, [IO.FileMode]::Create,
    [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $stream.SetLength($fixtureSize) } finally { $stream.Dispose() }

$process = $null
$secondary = $null
$passed = $false
try {
    $process = Start-Process -FilePath $executable -WindowStyle Hidden -PassThru
    [IntPtr]$window = Wait-MainWindow $process
    Start-Sleep -Milliseconds 500
    $beforeOpen = Get-MemorySample $process

    # The named-mutex route exercises the asynchronous OpenPath worker while
    # preserving a pre-open process baseline for meaningful memory deltas.
    $secondary = Start-Process -FilePath $executable -ArgumentList ('"{0}"' -f $fixture) `
        -WindowStyle Hidden -PassThru
    if (-not $secondary.WaitForExit(15000) -or $secondary.ExitCode -ne 0) {
        throw 'The forwarding process did not terminate cleanly.'
    }
    $fileName = [IO.Path]::GetFileName($fixture)
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        if ($process.HasExited) { throw 'BinEdit exited while opening the mapped fixture.' }
        if ([BinEditMemoryAutomation]::WindowText($window).Contains($fileName) -and
            [BinEditMemoryAutomation]::CloseTabEnabled($window)) { break }
        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not [BinEditMemoryAutomation]::WindowText($window).Contains($fileName)) {
        throw 'Timed out waiting for the mapped fixture to finish loading.'
    }

    $afterOpen = Get-MemorySample $process
    [long]$mappedGrowth = $afterOpen.MappedCommittedBytes - $beforeOpen.MappedCommittedBytes
    [long]$fixtureMappedBytes = [BinEditMemoryAutomation]::MappedFileBytes(
        [uint32]$process.Id, $fileName)
    [long]$privateGrowth = $afterOpen.PrivateBytes - $beforeOpen.PrivateBytes
    if ($fixtureMappedBytes -lt $minimumMappedGrowth) {
        throw "Only $fixtureMappedBytes bytes were mapped from the fixture file."
    }
    if ($privateGrowth -gt $maximumPrivateGrowth) {
        throw "Opening the mapped fixture added $privateGrowth private bytes."
    }

    [BinEditMemoryAutomation]::CloseTab($window)
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    $afterClose = $null
    [long]$mappedReleased = 0
    do {
        if ($process.HasExited) { throw 'Closing the final file terminated the main process.' }
        $afterClose = Get-MemorySample $process
        $mappedReleased = $fixtureMappedBytes -
            [long][BinEditMemoryAutomation]::MappedFileBytes([uint32]$process.Id, $fileName)
        if (-not [BinEditMemoryAutomation]::CloseTabEnabled($window) -and
            $mappedReleased -ge $minimumMappedGrowth) { break }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)

    if ([BinEditMemoryAutomation]::CloseTabEnabled($window)) {
        throw 'The final named document did not return to a pristine Untitled tab.'
    }
    if ($mappedReleased -lt $minimumMappedGrowth) {
        throw "Closing the mapped file released only $mappedReleased mapped bytes."
    }
    if ($afterClose.PrivateBytes - $beforeOpen.PrivateBytes -gt $maximumPrivateGrowth) {
        throw 'Closing the mapped document retained excessive private memory.'
    }

    [BinEditMemoryAutomation]::CloseWindow($window)
    if (-not $process.WaitForExit(5000) -or $process.ExitCode -ne 0) {
        throw 'BinEdit did not exit cleanly after the mapped-memory test.'
    }

    $passed = $true
    [pscustomobject]@{
        Result = 'PASS'
        Configuration = $Configuration
        FixtureBytes = $fixtureSize
        MappedBytesAdded = $mappedGrowth
        FixtureMappedBytes = $fixtureMappedBytes
        PrivateBytesAdded = $privateGrowth
        MappedBytesReleased = $mappedReleased
        UntitledPlaceholderCreated = $true
    } | Format-List
}
finally {
    if ($secondary -and -not $secondary.HasExited) { Stop-Process -Id $secondary.Id -Force }
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    if ($passed -and (Test-Path -LiteralPath $fixture)) {
        Remove-Item -LiteralPath $fixture -Force
    }
    if (-not $passed) { Write-Warning "Failed mapped-memory fixture retained: $fixture" }
}
