param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

# Verifies the special final-primary-tab rule for a modified untitled document.
# Ctrl+W must become available, enter the normal save/discard/cancel transaction,
# retain the dirty tab after Cancel, and replace discarded contents with a fresh
# untitled placeholder after No. Detach remains disabled throughout.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class BinEditUntitledCloseAutomation
{
    public const string MainWindowClass = "BinEdit.MainWindow";
    public const string MessageDialogClass = "BinEdit.MessageDialog.Gdi";
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_KEYUP = 0x0101;
    public const uint WM_CHAR = 0x0102;
    public const uint WM_INITMENUPOPUP = 0x0117;
    public const int VK_CONTROL = 0x11;
    public const int VK_ESCAPE = 0x1B;
    public const int VK_RIGHT = 0x27;
    public const int VK_RETURN = 0x0D;
    private const uint KEYEVENTF_KEYUP = 0x0002;
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
    private static extern int GetWindowTextW(IntPtr window, StringBuilder text, int maximumCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessageW(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool PostMessageW(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetMenu(IntPtr window);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetSubMenu(IntPtr menu, int position);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetMenuState(IntPtr menu, uint item, uint flags);

    [DllImport("user32.dll")]
    private static extern void keybd_event(byte virtualKey, byte scanCode, uint flags, UIntPtr extraInfo);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr window);

    [DllImport("user32.dll")]
    public static extern bool IsWindowEnabled(IntPtr window);

    public static IntPtr FindWindow(uint processId, string expectedClass)
    {
        IntPtr result = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr parameter)
        {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner != processId) return true;
            StringBuilder name = new StringBuilder(256);
            if (GetClassNameW(window, name, name.Capacity) > 0 && name.ToString() == expectedClass)
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

    public static bool CommandEnabled(IntPtr window, int topLevelPosition, uint command)
    {
        IntPtr menu = GetMenu(window);
        IntPtr popup = menu == IntPtr.Zero ? IntPtr.Zero : GetSubMenu(menu, topLevelPosition);
        if (popup == IntPtr.Zero) throw new InvalidOperationException("Could not resolve the File menu.");
        SendMessageW(window, WM_INITMENUPOPUP,
            new UIntPtr(unchecked((ulong)popup.ToInt64())), IntPtr.Zero);
        uint state = GetMenuState(popup, command, MF_BYCOMMAND);
        if (state == UInt32.MaxValue) throw new InvalidOperationException("Could not resolve command " + command + ".");
        return (state & (MF_GRAYED | MF_DISABLED)) == 0;
    }

    public static void Character(IntPtr window, char character)
    {
        SendMessageW(window, WM_CHAR, new UIntPtr(character), IntPtr.Zero);
    }

    public static void Key(IntPtr window, int virtualKey)
    {
        SendMessageW(window, WM_KEYDOWN, new UIntPtr((uint)virtualKey), IntPtr.Zero);
        SendMessageW(window, WM_KEYUP, new UIntPtr((uint)virtualKey), IntPtr.Zero);
    }

    public static void CtrlW(IntPtr window)
    {
        SetForegroundWindow(window);
        keybd_event(VK_CONTROL, 0, 0, UIntPtr.Zero);
        Thread.Sleep(20);
        // Post instead of send because Ctrl+W synchronously enters the modal
        // confirmation loop on the UI thread. SendMessage would keep this test
        // thread blocked until the very dialog it still needs to automate exits.
        PostMessageW(window, WM_KEYDOWN, new UIntPtr((uint)'W'), IntPtr.Zero);
        PostMessageW(window, WM_KEYUP, new UIntPtr((uint)'W'), IntPtr.Zero);
        keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
    }

    public static void Close(IntPtr window)
    {
        // Cleanup must also stay asynchronous in case a failed assertion left
        // a dirty document that would otherwise open another modal prompt.
        PostMessageW(window, WM_CLOSE, UIntPtr.Zero, IntPtr.Zero);
    }
}
'@

function Wait-BinEditWindow {
    param(
        [Diagnostics.Process]$Process,
        [string]$ClassName,
        [bool]$Present = $true,
        [int]$TimeoutSeconds = 10
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if ($Process.HasExited) { throw "BinEdit exited unexpectedly with code $($Process.ExitCode)." }
        [IntPtr]$window = [BinEditUntitledCloseAutomation]::FindWindow([uint32]$Process.Id, $ClassName)
        if ($Present -and $window -ne [IntPtr]::Zero) { return $window }
        if (-not $Present -and $window -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
        Start-Sleep -Milliseconds 20
    } while ([DateTime]::UtcNow -lt $deadline)
    $state = if ($Present) { 'appear' } else { 'close' }
    throw "Timed out waiting for window class '$ClassName' to $state."
}

$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root "x64\$Configuration\BinEdit.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Build output was not found: $executable"
}
$recoveryDirectory = Join-Path $env:LOCALAPPDATA 'BinEdit\Recovery'
$existingRecovery = @()
if (Test-Path -LiteralPath $recoveryDirectory -PathType Container) {
    $existingRecovery = @(Get-ChildItem -LiteralPath $recoveryDirectory -File -Force)
}
if ($existingRecovery.Count -ne 0) {
    throw 'The Ctrl+W test refused to run because pre-existing user recovery records must not be modified.'
}

$process = $null
$namedFixture = Join-Path ([IO.Path]::GetTempPath()) "BinEdit.NamedClose.$PID.bin"
$passed = $false
try {
    $process = Start-Process -FilePath $executable -PassThru
    [IntPtr]$editor = Wait-BinEditWindow $process ([BinEditUntitledCloseAutomation]::MainWindowClass)

    if ([BinEditUntitledCloseAutomation]::CommandEnabled($editor, 0, 40005)) {
        throw 'Close tab was enabled for the pristine final primary tab.'
    }
    [BinEditUntitledCloseAutomation]::Character($editor, 'A')
    [BinEditUntitledCloseAutomation]::Character($editor, '7')
    if (-not [BinEditUntitledCloseAutomation]::CommandEnabled($editor, 0, 40005)) {
        throw 'Close tab did not enable for the modified untitled final primary tab.'
    }
    if ([BinEditUntitledCloseAutomation]::CommandEnabled($editor, 0, 40006)) {
        throw 'Detach tab incorrectly enabled for the final primary tab.'
    }

    # Cancel must leave both the unsaved contents and the newly enabled Ctrl+W
    # path intact.
    [BinEditUntitledCloseAutomation]::CtrlW($editor)
    [IntPtr]$cancelPrompt = Wait-BinEditWindow $process ([BinEditUntitledCloseAutomation]::MessageDialogClass)
    if ([BinEditUntitledCloseAutomation]::IsWindowEnabled($editor)) {
        throw 'Save confirmation did not disable its owner.'
    }
    # Closing the dialog's system surface is the same CancelResult path as
    # Escape, while remaining reliable when this test runs without foreground
    # keyboard ownership on a busy desktop.
    [BinEditUntitledCloseAutomation]::Close($cancelPrompt)
    $null = Wait-BinEditWindow $process ([BinEditUntitledCloseAutomation]::MessageDialogClass) $false
    if (-not [BinEditUntitledCloseAutomation]::CommandEnabled($editor, 0, 40005)) {
        throw 'Cancel discarded or disabled the modified untitled tab.'
    }

    # The second Ctrl+W selects No. The main host remains alive with a new clean
    # untitled placeholder, for which Close tab returns to its disabled state.
    [BinEditUntitledCloseAutomation]::CtrlW($editor)
    [IntPtr]$discardPrompt = Wait-BinEditWindow $process ([BinEditUntitledCloseAutomation]::MessageDialogClass)
    [BinEditUntitledCloseAutomation]::Key($discardPrompt, [BinEditUntitledCloseAutomation]::VK_RIGHT)
    [BinEditUntitledCloseAutomation]::Key($discardPrompt, [BinEditUntitledCloseAutomation]::VK_RETURN)
    $null = Wait-BinEditWindow $process ([BinEditUntitledCloseAutomation]::MessageDialogClass) $false
    if ([BinEditUntitledCloseAutomation]::CommandEnabled($editor, 0, 40005)) {
        throw 'Close tab remained enabled after the discarded contents were replaced by a pristine tab.'
    }

    # End the first process, then verify the complementary final-tab case: one
    # clean named file closes directly through Ctrl+W and the main host returns
    # to a pristine untitled placeholder instead of ignoring the shortcut.
    [BinEditUntitledCloseAutomation]::Close($editor)
    if (-not $process.WaitForExit(5000) -or $process.ExitCode -ne 0) {
        throw 'The modified-untitled phase did not exit cleanly.'
    }
    [byte[]]$namedBytes = 0x10, 0x20, 0x30
    [IO.File]::WriteAllBytes($namedFixture, $namedBytes)
    $process = Start-Process -FilePath $executable -ArgumentList ('"{0}"' -f $namedFixture) -PassThru
    [IntPtr]$editor = Wait-BinEditWindow $process ([BinEditUntitledCloseAutomation]::MainWindowClass)
    $namedFileName = [IO.Path]::GetFileName($namedFixture)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline -and
           -not [BinEditUntitledCloseAutomation]::WindowText($editor).Contains($namedFileName)) {
        Start-Sleep -Milliseconds 10
    }
    if (-not [BinEditUntitledCloseAutomation]::WindowText($editor).Contains($namedFileName)) {
        throw 'Timed out waiting for the named document to finish loading.'
    }
    if (-not [BinEditUntitledCloseAutomation]::CommandEnabled($editor, 0, 40005)) {
        throw 'Close tab was disabled for the final named document.'
    }
    [BinEditUntitledCloseAutomation]::CtrlW($editor)
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        Start-Sleep -Milliseconds 20
        $closedToPlaceholder = -not [BinEditUntitledCloseAutomation]::CommandEnabled($editor, 0, 40005)
    } while (-not $closedToPlaceholder -and [DateTime]::UtcNow -lt $deadline)
    if (-not $closedToPlaceholder) {
        throw 'Ctrl+W did not replace the final named document with an untitled placeholder.'
    }
    if ([BinEditUntitledCloseAutomation]::FindWindow([uint32]$process.Id,
        [BinEditUntitledCloseAutomation]::MessageDialogClass) -ne [IntPtr]::Zero) {
        throw 'A clean named document unexpectedly requested save confirmation.'
    }
    [byte[]]$namedActual = [IO.File]::ReadAllBytes($namedFixture)
    if ($namedActual.Length -ne $namedBytes.Length) {
        throw 'Closing the clean named document changed its file length.'
    }
    for ($index = 0; $index -lt $namedBytes.Length; ++$index) {
        if ($namedActual[$index] -ne $namedBytes[$index]) {
            throw 'Closing the clean named document changed its file bytes.'
        }
    }

    $passed = $true
    [pscustomobject]@{
        Result = 'PASS'
        Configuration = $Configuration
        PristineFinalTabCloseDisabled = $true
        ModifiedUntitledCloseEnabled = $true
        CtrlWOpenedSaveConfirmation = $true
        CancelPreservedDirtyTab = $true
        DiscardCreatedFreshUntitledTab = $true
        FinalNamedTabCloseEnabled = $true
        NamedCtrlWCreatedUntitledPlaceholder = $true
        FinalPrimaryDetachDisabled = $true
    } | Format-List
}
finally {
    if ($process -and -not $process.HasExited) {
        [IntPtr]$editor = [BinEditUntitledCloseAutomation]::FindWindow(
            [uint32]$process.Id, [BinEditUntitledCloseAutomation]::MainWindowClass)
        if ($editor -ne [IntPtr]::Zero) { [BinEditUntitledCloseAutomation]::Close($editor) }
        $null = $process.WaitForExit(3000)
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    }
    # A failed run can be force-terminated while its dirty one-byte fixture has
    # already been checkpointed. Since the directory was proven empty before
    # this process acquired BinEdit's singleton mutex, these exact record names
    # belong to this run and are safe to remove as test cleanup.
    if (Test-Path -LiteralPath $recoveryDirectory -PathType Container) {
        Get-ChildItem -LiteralPath $recoveryDirectory -File -Force |
            Where-Object Name -Match '^[0-9a-f]{32}\.binrecovery$' |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
    }
    if ($passed -and (Test-Path -LiteralPath $namedFixture)) {
        Remove-Item -LiteralPath $namedFixture -Force
    }
    if (-not $passed) { Write-Warning 'The modified-untitled Ctrl+W test did not complete.' }
}
