param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateRange(8, 256)]
    [int]$DetachCycles = 48
)

# Measures the editor's complete committed address-space breakdown while repeatedly
# creating and closing detached D3D/DXGI hosts. It also verifies prompt release of
# a large document owned by a detached host and probes the Explorer taskbar thread
# throughout the run. The warm-up phase intentionally absorbs one-time DirectX and
# font-cache initialization before growth thresholds are evaluated.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public sealed class BinEditRegionSnapshot
{
    public ulong TotalCommittedBytes;
    public ulong PrivateCommittedBytes;
    public ulong MappedCommittedBytes;
    public ulong ImageCommittedBytes;
    public ulong GuardCommittedBytes;
    public ulong ExecutableCommittedBytes;
    public ulong RegionCount;
    public ulong PrivateRegionCount;
}

public sealed class BinEditSystemMemorySnapshot
{
    public ulong CommitTotalBytes;
    public ulong CommitLimitBytes;
    public ulong PhysicalTotalBytes;
    public ulong PhysicalAvailableBytes;
    public uint HandleCount;
    public uint ProcessCount;
    public uint ThreadCount;
}

public static class BinEditMemoryRegionAutomation
{
    public const string MainWindowClass = "BinEdit.MainWindow";
    public const string DetachedWindowClass = "BinEdit.DetachedTabWindow";
    public const uint WM_NULL = 0x0000;
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_COMMAND = 0x0111;
    public const uint SMTO_BLOCK = 0x0001;
    public const uint SMTO_ABORTIFHUNG = 0x0002;

    private const uint PROCESS_QUERY_INFORMATION = 0x0400;
    private const uint PROCESS_VM_READ = 0x0010;
    private const uint MEM_COMMIT = 0x1000;
    private const uint MEM_PRIVATE = 0x20000;
    private const uint MEM_MAPPED = 0x40000;
    private const uint MEM_IMAGE = 0x1000000;
    private const uint PAGE_GUARD = 0x100;
    private const uint PAGE_EXECUTE = 0x10;
    private const uint PAGE_EXECUTE_READ = 0x20;
    private const uint PAGE_EXECUTE_READWRITE = 0x40;
    private const uint PAGE_EXECUTE_WRITECOPY = 0x80;

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

    [StructLayout(LayoutKind.Sequential)]
    private struct PERFORMANCE_INFORMATION
    {
        public uint cb;
        public UIntPtr CommitTotal;
        public UIntPtr CommitLimit;
        public UIntPtr CommitPeak;
        public UIntPtr PhysicalTotal;
        public UIntPtr PhysicalAvailable;
        public UIntPtr SystemCache;
        public UIntPtr KernelTotal;
        public UIntPtr KernelPaged;
        public UIntPtr KernelNonpaged;
        public UIntPtr PageSize;
        public uint HandleCount;
        public uint ProcessCount;
        public uint ThreadCount;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetClassNameW(IntPtr window, StringBuilder className, int maximumCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetWindowTextW(IntPtr window, StringBuilder text, int maximumCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr FindWindowW(string className, string windowName);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessageW(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SendMessageTimeoutW(IntPtr window, uint message,
        UIntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetGuiResources(IntPtr process, uint flags);

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

    [DllImport("psapi.dll", SetLastError = true)]
    private static extern bool GetPerformanceInfo(ref PERFORMANCE_INFORMATION information, uint size);

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
            if (className == MainWindowClass || className == DetachedWindowClass)
                result.Add(window);
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

    public static void Command(IntPtr window, uint command)
    {
        SendMessageW(window, WM_COMMAND, new UIntPtr(command), IntPtr.Zero);
    }

    public static void CloseWindow(IntPtr window)
    {
        SendMessageW(window, WM_CLOSE, UIntPtr.Zero, IntPtr.Zero);
    }

    public static IntPtr TaskbarWindow()
    {
        return FindWindowW("Shell_TrayWnd", null);
    }

    public static uint WindowProcessId(IntPtr window)
    {
        uint processId;
        GetWindowThreadProcessId(window, out processId);
        return processId;
    }

    public static bool IsResponsive(IntPtr window, uint timeoutMilliseconds)
    {
        if (window == IntPtr.Zero) return false;
        UIntPtr ignored;
        return SendMessageTimeoutW(window, WM_NULL, UIntPtr.Zero, IntPtr.Zero,
            SMTO_BLOCK | SMTO_ABORTIFHUNG, timeoutMilliseconds, out ignored) != IntPtr.Zero;
    }

    public static uint GuiResources(uint processId, bool userObjects)
    {
        IntPtr process = OpenProcess(PROCESS_QUERY_INFORMATION, false, processId);
        if (process == IntPtr.Zero)
            throw new InvalidOperationException("OpenProcess failed for GUI resource sampling.");
        try { return GetGuiResources(process, userObjects ? 1u : 0u); }
        finally { CloseHandle(process); }
    }

    public static BinEditRegionSnapshot CaptureRegions(uint processId)
    {
        IntPtr process = OpenProcess(PROCESS_QUERY_INFORMATION, false, processId);
        if (process == IntPtr.Zero)
            throw new InvalidOperationException("OpenProcess failed for VirtualQueryEx sampling.");
        try
        {
            BinEditRegionSnapshot result = new BinEditRegionSnapshot();
            ulong address = 0;
            ulong maximumAddress = IntPtr.Size == 8 ? 0x00007FFFFFFF0000UL : 0xFFF00000UL;
            UIntPtr structureSize = new UIntPtr((uint)Marshal.SizeOf(typeof(MEMORY_BASIC_INFORMATION)));
            while (address < maximumAddress)
            {
                MEMORY_BASIC_INFORMATION information;
                UIntPtr queried = VirtualQueryEx(process, new IntPtr(unchecked((long)address)),
                    out information, structureSize);
                if (queried == UIntPtr.Zero) break;
                ulong baseAddress = unchecked((ulong)information.BaseAddress.ToInt64());
                ulong regionSize = information.RegionSize.ToUInt64();
                if (regionSize == 0) break;
                if (information.State == MEM_COMMIT)
                {
                    result.RegionCount++;
                    result.TotalCommittedBytes += regionSize;
                    if (information.Type == MEM_PRIVATE)
                    {
                        result.PrivateCommittedBytes += regionSize;
                        result.PrivateRegionCount++;
                    }
                    else if (information.Type == MEM_MAPPED) result.MappedCommittedBytes += regionSize;
                    else if (information.Type == MEM_IMAGE) result.ImageCommittedBytes += regionSize;
                    if ((information.Protect & PAGE_GUARD) != 0) result.GuardCommittedBytes += regionSize;
                    uint baseProtection = information.Protect & 0xFFu;
                    if (baseProtection == PAGE_EXECUTE || baseProtection == PAGE_EXECUTE_READ ||
                        baseProtection == PAGE_EXECUTE_READWRITE || baseProtection == PAGE_EXECUTE_WRITECOPY)
                        result.ExecutableCommittedBytes += regionSize;
                }
                ulong next = baseAddress + regionSize;
                if (next <= address) break;
                address = next;
            }
            return result;
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

    public static BinEditSystemMemorySnapshot CaptureSystemMemory()
    {
        PERFORMANCE_INFORMATION information = new PERFORMANCE_INFORMATION();
        information.cb = (uint)Marshal.SizeOf(typeof(PERFORMANCE_INFORMATION));
        if (!GetPerformanceInfo(ref information, information.cb))
            throw new InvalidOperationException("GetPerformanceInfo failed.");
        ulong pageSize = information.PageSize.ToUInt64();
        BinEditSystemMemorySnapshot result = new BinEditSystemMemorySnapshot();
        result.CommitTotalBytes = information.CommitTotal.ToUInt64() * pageSize;
        result.CommitLimitBytes = information.CommitLimit.ToUInt64() * pageSize;
        result.PhysicalTotalBytes = information.PhysicalTotal.ToUInt64() * pageSize;
        result.PhysicalAvailableBytes = information.PhysicalAvailable.ToUInt64() * pageSize;
        result.HandleCount = information.HandleCount;
        result.ProcessCount = information.ProcessCount;
        result.ThreadCount = information.ThreadCount;
        return result;
    }
}
'@

function Wait-EditorWindowCount {
    param([Diagnostics.Process]$Process, [int]$Count, [int]$TimeoutSeconds = 15)

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $lastCount = -1
    do {
        if ($Process.HasExited) { throw "BinEdit exited unexpectedly with code $($Process.ExitCode)." }
        $windows = [BinEditMemoryRegionAutomation]::FindEditorWindows([uint32]$Process.Id)
        $lastCount = $windows.Length
        if ($lastCount -eq $Count) { return $windows }
        Start-Sleep -Milliseconds 20
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for $Count editor window(s); last observed count was $lastCount."
}

function Get-MainWindow {
    param([IntPtr[]]$Windows)

    foreach ($window in $Windows) {
        if ([BinEditMemoryRegionAutomation]::ClassName($window) -eq
            [BinEditMemoryRegionAutomation]::MainWindowClass) { return $window }
    }
    throw 'The main editor window was not found.'
}

function Get-DetachedWindow {
    param([IntPtr[]]$Windows)

    foreach ($window in $Windows) {
        if ([BinEditMemoryRegionAutomation]::ClassName($window) -eq
            [BinEditMemoryRegionAutomation]::DetachedWindowClass) { return $window }
    }
    throw 'The detached editor window was not found.'
}

function Assert-Responsive {
    param([IntPtr]$Window, [string]$Label, [uint32]$TimeoutMilliseconds = 1000)

    if (-not [BinEditMemoryRegionAutomation]::IsResponsive($Window, $TimeoutMilliseconds)) {
        throw "$Label did not process WM_NULL within $TimeoutMilliseconds ms."
    }
}

function Get-ProcessSnapshot {
    param([Diagnostics.Process]$Process)

    $Process.Refresh()
    $regions = [BinEditMemoryRegionAutomation]::CaptureRegions([uint32]$Process.Id)
    [pscustomobject]@{
        PrivateBytes = [long]$Process.PrivateMemorySize64
        WorkingSetBytes = [long]$Process.WorkingSet64
        VirtualBytes = [long]$Process.VirtualMemorySize64
        HandleCount = [int]$Process.HandleCount
        GdiObjects = [int][BinEditMemoryRegionAutomation]::GuiResources([uint32]$Process.Id, $false)
        UserObjects = [int][BinEditMemoryRegionAutomation]::GuiResources([uint32]$Process.Id, $true)
        TotalCommittedBytes = [long]$regions.TotalCommittedBytes
        PrivateCommittedBytes = [long]$regions.PrivateCommittedBytes
        MappedCommittedBytes = [long]$regions.MappedCommittedBytes
        ImageCommittedBytes = [long]$regions.ImageCommittedBytes
        GuardCommittedBytes = [long]$regions.GuardCommittedBytes
        ExecutableCommittedBytes = [long]$regions.ExecutableCommittedBytes
        RegionCount = [long]$regions.RegionCount
        PrivateRegionCount = [long]$regions.PrivateRegionCount
    }
}

function Get-ShellSnapshot {
    param([uint32]$ExpectedProcessId)

    [IntPtr]$taskbar = [BinEditMemoryRegionAutomation]::TaskbarWindow()
    if ($taskbar -eq [IntPtr]::Zero) { throw 'Shell_TrayWnd was not found.' }
    [uint32]$actualProcessId = [BinEditMemoryRegionAutomation]::WindowProcessId($taskbar)
    if ($ExpectedProcessId -ne 0 -and $actualProcessId -ne $ExpectedProcessId) {
        throw "Explorer/taskbar restarted during the test ($ExpectedProcessId -> $actualProcessId)."
    }
    Assert-Responsive $taskbar 'Explorer taskbar'
    $process = [Diagnostics.Process]::GetProcessById([int]$actualProcessId)
    $process.Refresh()
    [pscustomobject]@{
        Window = $taskbar
        ProcessId = $actualProcessId
        SessionId = $process.SessionId
        PrivateBytes = [long]$process.PrivateMemorySize64
        WorkingSetBytes = [long]$process.WorkingSet64
        HandleCount = [int]$process.HandleCount
        GdiObjects = [int][BinEditMemoryRegionAutomation]::GuiResources($actualProcessId, $false)
        UserObjects = [int][BinEditMemoryRegionAutomation]::GuiResources($actualProcessId, $true)
    }
}

function Get-DesktopProcessSnapshot {
    param([string]$Name, [int]$SessionId, [uint32]$ExpectedProcessId = 0)

    $process = Get-Process -Name $Name -ErrorAction Stop |
        Where-Object { $_.SessionId -eq $SessionId } | Select-Object -First 1
    if (-not $process) { throw "The $Name process for session $SessionId was not found." }
    if ($ExpectedProcessId -ne 0 -and $process.Id -ne $ExpectedProcessId) {
        throw "$Name restarted during the test ($ExpectedProcessId -> $($process.Id))."
    }
    $process.Refresh()
    [pscustomobject]@{
        ProcessId = [uint32]$process.Id
        PrivateBytes = [long]$process.PrivateMemorySize64
        WorkingSetBytes = [long]$process.WorkingSet64
        HandleCount = [int]$process.HandleCount
    }
}

function Invoke-DetachCloseCycle {
    param([Diagnostics.Process]$Process, [IntPtr]$MainWindow, [IntPtr]$TaskbarWindow)

    # A second clean tab makes the command valid without introducing file I/O or
    # save prompts, isolating controller, HWND, DirectX, and composition lifetime.
    [BinEditMemoryRegionAutomation]::Command($MainWindow, 40000)
    [BinEditMemoryRegionAutomation]::Command($MainWindow, 40006)
    $windows = Wait-EditorWindowCount $Process 2
    [IntPtr]$detached = Get-DetachedWindow $windows
    Assert-Responsive $MainWindow 'main editor window'
    Assert-Responsive $detached 'detached editor window'
    Assert-Responsive $TaskbarWindow 'Explorer taskbar'
    [BinEditMemoryRegionAutomation]::CloseWindow($detached)
    [void](Wait-EditorWindowCount $Process 1)
}

$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root "x64\$Configuration\BinEdit.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Build output was not found: $executable"
}

$largeFixtureSize = 96L * 1024L * 1024L
$minimumLargeMappedBytes = 88L * 1024L * 1024L
$largeFixture = Join-Path ([IO.Path]::GetTempPath()) "BinEdit.DetachedMemory.$PID.large.bin"
$smallFixture = Join-Path ([IO.Path]::GetTempPath()) "BinEdit.DetachedMemory.$PID.small.bin"
$stream = [IO.File]::Open($largeFixture, [IO.FileMode]::Create,
    [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $stream.SetLength($largeFixtureSize) } finally { $stream.Dispose() }
[IO.File]::WriteAllBytes($smallFixture, [byte[]](0x42, 0x69, 0x6E))

$process = $null
$largeProcess = $null
$passed = $false
try {
    $shellBefore = Get-ShellSnapshot 0
    $dwmBefore = Get-DesktopProcessSnapshot 'dwm' $shellBefore.SessionId
    $systemBefore = [BinEditMemoryRegionAutomation]::CaptureSystemMemory()

    $process = Start-Process -FilePath $executable -WindowStyle Hidden -PassThru
    $windows = Wait-EditorWindowCount $process 1
    [IntPtr]$mainWindow = Get-MainWindow $windows
    Assert-Responsive $mainWindow 'main editor window after startup'

    # Six warm-up cycles initialize process-wide D3D, DXGI, DirectWrite, font,
    # menu, and desktop-composition caches before the leak-sensitive baseline.
    for ($cycle = 1; $cycle -le 6; ++$cycle) {
        Invoke-DetachCloseCycle $process $mainWindow $shellBefore.Window
    }
    Start-Sleep -Milliseconds 750
    $baseline = Get-ProcessSnapshot $process
    $cycleSamples = [Collections.Generic.List[object]]::new()

    for ($cycle = 1; $cycle -le $DetachCycles; ++$cycle) {
        Invoke-DetachCloseCycle $process $mainWindow $shellBefore.Window
        if (($cycle % 8) -eq 0 -or $cycle -eq $DetachCycles) {
            Start-Sleep -Milliseconds 100
            $sample = Get-ProcessSnapshot $process
            $cycleSamples.Add([pscustomobject]@{
                Cycle = $cycle
                PrivateMiB = [Math]::Round($sample.PrivateBytes / 1MB, 2)
                PrivateCommittedMiB = [Math]::Round($sample.PrivateCommittedBytes / 1MB, 2)
                Handles = $sample.HandleCount
                Gdi = $sample.GdiObjects
                User = $sample.UserObjects
                Regions = $sample.RegionCount
            })
        }
    }
    Start-Sleep -Milliseconds 1000
    $afterCycles = Get-ProcessSnapshot $process

    [long]$privateGrowth = $afterCycles.PrivateBytes - $baseline.PrivateBytes
    [long]$privateCommitGrowth = $afterCycles.PrivateCommittedBytes - $baseline.PrivateCommittedBytes
    [long]$totalCommitGrowth = $afterCycles.TotalCommittedBytes - $baseline.TotalCommittedBytes
    [int]$handleGrowth = $afterCycles.HandleCount - $baseline.HandleCount
    [int]$gdiGrowth = $afterCycles.GdiObjects - $baseline.GdiObjects
    [int]$userGrowth = $afterCycles.UserObjects - $baseline.UserObjects

    if ($privateGrowth -gt 64MB) {
        throw "Detached-host stress retained $privateGrowth private bytes after $DetachCycles measured cycles."
    }
    if ($privateCommitGrowth -gt 64MB -or $totalCommitGrowth -gt 96MB) {
        throw "VirtualQueryEx found unbounded committed growth: private=$privateCommitGrowth, total=$totalCommitGrowth."
    }
    if ($handleGrowth -gt 64 -or $gdiGrowth -gt 16 -or $userGrowth -gt 16) {
        throw "Resource growth exceeded limits: handles=$handleGrowth, GDI=$gdiGrowth, USER=$userGrowth."
    }

    [BinEditMemoryRegionAutomation]::CloseWindow($mainWindow)
    if (-not $process.WaitForExit(5000) -or $process.ExitCode -ne 0) {
        throw 'The detached-host stress process did not exit cleanly.'
    }

    # Load a large document as the active second tab, detach it, and close that
    # host. This directly proves the closed controller releases document storage
    # and undo capacity instead of retaining them in the root ownership vector.
    $arguments = @(('"{0}"' -f $smallFixture), ('"{0}"' -f $largeFixture))
    $largeProcess = Start-Process -FilePath $executable -ArgumentList $arguments `
        -WindowStyle Hidden -PassThru
    $windows = Wait-EditorWindowCount $largeProcess 1 30
    [IntPtr]$largeMain = Get-MainWindow $windows
    $largeName = [IO.Path]::GetFileName($largeFixture)
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        if ($largeProcess.HasExited) { throw 'BinEdit exited while loading the large detached fixture.' }
        if ([BinEditMemoryRegionAutomation]::WindowText($largeMain).Contains($largeName)) { break }
        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not [BinEditMemoryRegionAutomation]::WindowText($largeMain).Contains($largeName)) {
        throw 'Timed out waiting for the large detached fixture to finish loading.'
    }

    [BinEditMemoryRegionAutomation]::Command($largeMain, 40006)
    $windows = Wait-EditorWindowCount $largeProcess 2
    [IntPtr]$largeDetached = Get-DetachedWindow $windows
    if (-not [BinEditMemoryRegionAutomation]::WindowText($largeDetached).Contains($largeName)) {
        throw 'The large active tab was not transferred to the detached host.'
    }
    [long]$mappedBeforeLargeClose = [BinEditMemoryRegionAutomation]::MappedFileBytes(
        [uint32]$largeProcess.Id, $largeName)
    if ($mappedBeforeLargeClose -lt $minimumLargeMappedBytes) {
        throw "The detached large file has only $mappedBeforeLargeClose mapped bytes."
    }
    [BinEditMemoryRegionAutomation]::CloseWindow($largeDetached)
    [void](Wait-EditorWindowCount $largeProcess 1)

    $deadline = [DateTime]::UtcNow.AddSeconds(12)
    [long]$mappedAfterLargeClose = $mappedBeforeLargeClose
    do {
        $mappedAfterLargeClose = [long][BinEditMemoryRegionAutomation]::MappedFileBytes(
            [uint32]$largeProcess.Id, $largeName)
        if ($mappedAfterLargeClose -eq 0) { break }
        Start-Sleep -Milliseconds 75
    } while ([DateTime]::UtcNow -lt $deadline)

    [long]$largeMappedReleased = $mappedBeforeLargeClose - $mappedAfterLargeClose
    if ($mappedAfterLargeClose -ne 0) {
        throw "Closing the large detached host retained $mappedAfterLargeClose mapped fixture bytes."
    }

    [BinEditMemoryRegionAutomation]::CloseWindow($largeMain)
    if (-not $largeProcess.WaitForExit(5000) -or $largeProcess.ExitCode -ne 0) {
        throw 'The large-document memory process did not exit cleanly.'
    }

    # Let Explorer and DWM retire the final composition frame before collecting
    # their end samples. Process identity checks detect either component restart.
    Start-Sleep -Milliseconds 1000
    $shellAfter = Get-ShellSnapshot ([uint32]$shellBefore.ProcessId)
    $dwmAfter = Get-DesktopProcessSnapshot 'dwm' $shellBefore.SessionId `
        ([uint32]$dwmBefore.ProcessId)
    $systemAfter = [BinEditMemoryRegionAutomation]::CaptureSystemMemory()
    Assert-Responsive $shellAfter.Window 'Explorer taskbar after all memory tests'
    [long]$explorerPrivateGrowth = $shellAfter.PrivateBytes - $shellBefore.PrivateBytes
    [int]$explorerHandleGrowth = $shellAfter.HandleCount - $shellBefore.HandleCount
    [int]$explorerGdiGrowth = $shellAfter.GdiObjects - $shellBefore.GdiObjects
    [int]$explorerUserGrowth = $shellAfter.UserObjects - $shellBefore.UserObjects
    if ($explorerPrivateGrowth -gt 64MB -or $explorerHandleGrowth -gt 96 -or
        $explorerGdiGrowth -gt 16 -or $explorerUserGrowth -gt 24) {
        throw "Explorer taskbar resources did not settle: private=$explorerPrivateGrowth, handles=$explorerHandleGrowth, GDI=$explorerGdiGrowth, USER=$explorerUserGrowth."
    }

    Write-Host 'Detached-host memory samples:'
    $cycleSamples | Format-Table -AutoSize
    $passed = $true
    [pscustomobject]@{
        Result = 'PASS'
        Configuration = $Configuration
        WarmupCycles = 6
        MeasuredDetachCycles = $DetachCycles
        PrivateGrowthBytes = $privateGrowth
        PrivateCommittedGrowthBytes = $privateCommitGrowth
        TotalCommittedGrowthBytes = $totalCommitGrowth
        HandleGrowth = $handleGrowth
        GdiObjectGrowth = $gdiGrowth
        UserObjectGrowth = $userGrowth
        LargeDetachedMappedBytesBeforeClose = $mappedBeforeLargeClose
        LargeDetachedMappedBytesReleased = $largeMappedReleased
        ExplorerProcessId = $shellAfter.ProcessId
        ExplorerPrivateBytesDelta = $explorerPrivateGrowth
        ExplorerHandleDelta = $explorerHandleGrowth
        ExplorerGdiDelta = $explorerGdiGrowth
        ExplorerUserDelta = $explorerUserGrowth
        DwmProcessId = $dwmAfter.ProcessId
        DwmPrivateBytesDelta = $dwmAfter.PrivateBytes - $dwmBefore.PrivateBytes
        DwmWorkingSetBytesDelta = $dwmAfter.WorkingSetBytes - $dwmBefore.WorkingSetBytes
        DwmHandleDelta = $dwmAfter.HandleCount - $dwmBefore.HandleCount
        SystemCommitBytesDelta = [long]$systemAfter.CommitTotalBytes - [long]$systemBefore.CommitTotalBytes
        TaskbarResponsive = $true
    } | Format-List
}
finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    if ($largeProcess -and -not $largeProcess.HasExited) { Stop-Process -Id $largeProcess.Id -Force }
    if ($passed) {
        if (Test-Path -LiteralPath $largeFixture) { Remove-Item -LiteralPath $largeFixture -Force }
        if (Test-Path -LiteralPath $smallFixture) { Remove-Item -LiteralPath $smallFixture -Force }
    }
    else {
        Write-Warning "Failed memory-region fixtures retained: $largeFixture, $smallFixture"
    }
}
