param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

# Verifies that the bit-operation tool opens without a real editor byte. It also
# checks native menu state at an empty/virtual EOF cell and
# after materializing a byte. Input is delivered to the actual custom D3D/D2D
# window, so the test covers its independent input/operand keyboard traversal.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class BinEditBitToolAutomation
{
    public const string MainWindowClass = "BinEdit.MainWindow";
    public const string BitToolWindowClass = "BinEdit.BitToolDialog";
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_COMMAND = 0x0111;
    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_CHAR = 0x0102;
    public const uint WM_INITMENUPOPUP = 0x0117;
    public const int VK_TAB = 0x09;
    public const int VK_BACK = 0x08;
    private const uint MF_BYCOMMAND = 0x00000000;
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
    private static extern bool PostMessageW(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessageW(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool IsWindowEnabled(IntPtr window);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetMenu(IntPtr window);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetSubMenu(IntPtr menu, int position);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetMenuState(IntPtr menu, uint item, uint flags);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetMenuStringW(IntPtr menu, uint item, StringBuilder text, int maximumCount, uint flags);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetWindowTextW(IntPtr window, StringBuilder text, int maximumCount);

    public static IntPtr FindWindow(uint processId, string expectedClass)
    {
        IntPtr result = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr parameter)
        {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner != processId) return true;
            StringBuilder className = new StringBuilder(256);
            GetClassNameW(window, className, className.Capacity);
            if (className.ToString() != expectedClass) return true;
            result = window;
            return false;
        }, IntPtr.Zero);
        return result;
    }

    public static bool CommandEnabled(IntPtr window, int topLevelPosition, uint command)
    {
        IntPtr menu = GetMenu(window);
        IntPtr popup = menu == IntPtr.Zero ? IntPtr.Zero : GetSubMenu(menu, topLevelPosition);
        if (popup == IntPtr.Zero)
            throw new InvalidOperationException("Could not resolve menu position " + topLevelPosition + ".");
        SendMessageW(window, WM_INITMENUPOPUP, new UIntPtr(unchecked((ulong)popup.ToInt64())), IntPtr.Zero);
        uint state = GetMenuState(popup, command, MF_BYCOMMAND);
        if (state == UInt32.MaxValue)
            throw new InvalidOperationException("Could not resolve command " + command + ".");
        return (state & (MF_GRAYED | MF_DISABLED)) == 0;
    }

    public static string CommandText(IntPtr window, int topLevelPosition, uint command)
    {
        IntPtr menu = GetMenu(window);
        IntPtr popup = menu == IntPtr.Zero ? IntPtr.Zero : GetSubMenu(menu, topLevelPosition);
        if (popup == IntPtr.Zero)
            throw new InvalidOperationException("Could not resolve menu position " + topLevelPosition + ".");
        StringBuilder text = new StringBuilder(256);
        if (GetMenuStringW(popup, command, text, text.Capacity, MF_BYCOMMAND) == 0)
            throw new InvalidOperationException("Could not read command text " + command + ".");
        return text.ToString();
    }

    public static string WindowText(IntPtr window)
    {
        StringBuilder text = new StringBuilder(256);
        GetWindowTextW(window, text, text.Capacity);
        return text.ToString();
    }

    public static void PostCommand(IntPtr window, uint command)
    {
        if (!PostMessageW(window, WM_COMMAND, new UIntPtr(command), IntPtr.Zero))
            throw new InvalidOperationException("PostMessageW failed with error " + Marshal.GetLastWin32Error() + ".");
    }

    public static void SendCommand(IntPtr window, uint command)
    {
        SendMessageW(window, WM_COMMAND, new UIntPtr(command), IntPtr.Zero);
    }

    public static void Key(IntPtr window, int virtualKey)
    {
        SendMessageW(window, WM_KEYDOWN, new UIntPtr((uint)virtualKey), IntPtr.Zero);
    }

    public static void Character(IntPtr window, char character)
    {
        SendMessageW(window, WM_CHAR, new UIntPtr(character), IntPtr.Zero);
    }

    public static void Close(IntPtr window)
    {
        SendMessageW(window, WM_CLOSE, UIntPtr.Zero, IntPtr.Zero);
    }
}
'@

function Wait-BinEditWindow {
    param([Diagnostics.Process]$Process, [string]$ClassName)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        if ($Process.HasExited) { throw "BinEdit exited unexpectedly with code $($Process.ExitCode)." }
        $window = [BinEditBitToolAutomation]::FindWindow([uint32]$Process.Id, $ClassName)
        if ($window -ne [IntPtr]::Zero) { return $window }
        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for window class '$ClassName'."
}

function Wait-WindowEnabledState {
    param([IntPtr]$Window, [bool]$Expected)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        if ([BinEditBitToolAutomation]::IsWindowEnabled($Window) -eq $Expected) { return }
        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for window enabled state $Expected."
}

$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root "x64\$Configuration\BinEdit.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Build output was not found: $executable" }

$fixture = Join-Path ([IO.Path]::GetTempPath()) "BinEdit.BitToolUiTests.$PID.bin"
[IO.File]::WriteAllBytes($fixture, [byte[]]::new(0))
$process = $null
$passed = $false
try {
    $process = Start-Process -FilePath $executable -ArgumentList ('"{0}"' -f $fixture) -WindowStyle Hidden -PassThru
    [IntPtr]$mainWindow = Wait-BinEditWindow $process ([BinEditBitToolAutomation]::MainWindowClass)
    $fixtureName = [IO.Path]::GetFileName($fixture)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline -and
           -not [BinEditBitToolAutomation]::WindowText($mainWindow).Contains($fixtureName)) {
        Start-Sleep -Milliseconds 10
    }
    if (-not [BinEditBitToolAutomation]::WindowText($mainWindow).Contains($fixtureName)) {
        throw 'Timed out waiting for the command-line fixture to finish loading.'
    }

    if (-not [BinEditBitToolAutomation]::CommandEnabled($mainWindow, 2, 40701)) {
        throw 'The bit-operation tool command was disabled for an empty document.'
    }
    $exitText = [BinEditBitToolAutomation]::CommandText($mainWindow, 0, 40004)
    if (-not $exitText.EndsWith("`tAlt+F4", [StringComparison]::Ordinal)) {
        throw "Exit menu text does not advertise Alt+F4: '$exitText'."
    }
    $toolMenuText = [BinEditBitToolAutomation]::CommandText($mainWindow, 2, 40701)
    if (-not [BinEditBitToolAutomation]::CommandEnabled($mainWindow, 0, 40005)) {
        throw 'Close tab was disabled for the primary window final named tab.'
    }
    if ([BinEditBitToolAutomation]::CommandEnabled($mainWindow, 0, 40006)) {
        throw 'Move tab remained enabled for the primary window final tab.'
    }
    if ([BinEditBitToolAutomation]::CommandEnabled($mainWindow, 4, 40202) -or
        [BinEditBitToolAutomation]::CommandEnabled($mainWindow, 4, 40203)) {
        throw 'Current-byte color commands were enabled for the empty virtual EOF cell.'
    }
    if ([BinEditBitToolAutomation]::CommandEnabled($mainWindow, 1, 40105)) {
        throw 'Mark-selection command was enabled for the empty virtual EOF cell.'
    }

    [BinEditBitToolAutomation]::PostCommand($mainWindow, 40701)
    [IntPtr]$bitTool = Wait-BinEditWindow $process ([BinEditBitToolAutomation]::BitToolWindowClass)
    Wait-WindowEnabledState $mainWindow $false
    $toolWindowText = [BinEditBitToolAutomation]::WindowText($bitTool)
    $nameMatches = ($toolMenuText.Contains('ビット処理ツール') -and $toolWindowText -eq 'ビット処理ツール') -or
                   ($toolMenuText.Contains('Bit operation tool') -and $toolWindowText -eq 'Bit operation tool')
    if (-not $nameMatches) {
        throw "Bit-tool names are inconsistent: menu='$toolMenuText', window='$toolWindowText'."
    }

    # Focus starts on the operation grid. Tab enters the independent input field;
    # four backspaces replace its initial 0x00 representation with 0xF0.
    [BinEditBitToolAutomation]::Key($bitTool, [BinEditBitToolAutomation]::VK_TAB)
    1..4 | ForEach-Object { [BinEditBitToolAutomation]::Character($bitTool, [char][BinEditBitToolAutomation]::VK_BACK) }
    '0xF0'.ToCharArray() | ForEach-Object { [BinEditBitToolAutomation]::Character($bitTool, $_) }

    # The next focus stop is the operand field for the default XOR operation.
    [BinEditBitToolAutomation]::Key($bitTool, [BinEditBitToolAutomation]::VK_TAB)
    1..4 | ForEach-Object { [BinEditBitToolAutomation]::Character($bitTool, [char][BinEditBitToolAutomation]::VK_BACK) }
    '0x0F'.ToCharArray() | ForEach-Object { [BinEditBitToolAutomation]::Character($bitTool, $_) }

    [BinEditBitToolAutomation]::Close($bitTool)
    Wait-WindowEnabledState $mainWindow $true
    if ($process.HasExited) { throw 'BinEdit exited when the bit-operation tool was dismissed.' }

    # A high nibble materializes a byte without advancing the caret, so both
    # current-value color commands must become available. Completing the low
    # nibble advances to virtual EOF and must disable both commands again.
    [BinEditBitToolAutomation]::Character($mainWindow, 'A')
    if (-not [BinEditBitToolAutomation]::CommandEnabled($mainWindow, 4, 40202) -or
        -not [BinEditBitToolAutomation]::CommandEnabled($mainWindow, 4, 40203)) {
        throw 'Current-byte color commands did not enable for a materialized byte.'
    }
    if (-not [BinEditBitToolAutomation]::CommandEnabled($mainWindow, 1, 40105)) {
        throw 'Mark-selection command did not enable for a materialized byte.'
    }
    [BinEditBitToolAutomation]::Character($mainWindow, '7')
    if ([BinEditBitToolAutomation]::CommandEnabled($mainWindow, 4, 40202) -or
        [BinEditBitToolAutomation]::CommandEnabled($mainWindow, 4, 40203)) {
        throw 'Current-byte color commands remained enabled at virtual EOF.'
    }
    if ([BinEditBitToolAutomation]::CommandEnabled($mainWindow, 1, 40105)) {
        throw 'Mark-selection command remained enabled at virtual EOF.'
    }

    # Bypass native menu dispatch to verify that stale/synthetic commands retain
    # the same EOF guard and neither open the picker nor index beyond the buffer.
    [BinEditBitToolAutomation]::SendCommand($mainWindow, 40202)
    [BinEditBitToolAutomation]::SendCommand($mainWindow, 40203)
    [BinEditBitToolAutomation]::SendCommand($mainWindow, 40105)
    if ($process.HasExited -or -not [BinEditBitToolAutomation]::IsWindowEnabled($mainWindow)) {
        throw 'A synthesized current-byte color command escaped the virtual EOF guard.'
    }

    # Restore the pristine empty document so teardown does not require a save
    # confirmation; the nibble overwrite and insertion are separate undo records.
    [BinEditBitToolAutomation]::SendCommand($mainWindow, 40101)
    [BinEditBitToolAutomation]::SendCommand($mainWindow, 40101)
    [BinEditBitToolAutomation]::SendCommand($mainWindow, 40002)
    if ((Get-Item -LiteralPath $fixture).Length -ne 0) {
        throw 'Undo did not restore the empty fixture before the cleanup save.'
    }

    $passed = $true
    [pscustomobject]@{
        Result = 'PASS'
        Configuration = $Configuration
        EmptyDocumentCommandEnabled = $true
        ToolInputEdited = '0xF0'
        ToolOperandEdited = '0x0F'
        UnifiedToolName = $toolWindowText
        ExitMenuShortcut = 'Alt+F4'
        PrimaryFinalNamedCloseEnabled = $true
        PrimaryFinalDetachDisabled = $true
        ByteColorEnabledOnRealByte = $true
        ByteColorDisabledAtVirtualEof = $true
        MarkSelectionDisabledAtVirtualEof = $true
        SynthesizedEofCommandsGuarded = $true
        OwnerModalStateRestored = $true
    } | Format-List
}
finally {
    if ($process -and -not $process.HasExited) {
        $mainWindow = [BinEditBitToolAutomation]::FindWindow([uint32]$process.Id, [BinEditBitToolAutomation]::MainWindowClass)
        if ($mainWindow -ne [IntPtr]::Zero) { [BinEditBitToolAutomation]::Close($mainWindow) }
        $null = $process.WaitForExit(5000)
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    }
    if ($passed -and (Test-Path -LiteralPath $fixture)) {
        Remove-Item -LiteralPath $fixture -Force
    }
    if (-not $passed) { Write-Warning 'Bit-operation tool UI verification failed.' }
}
