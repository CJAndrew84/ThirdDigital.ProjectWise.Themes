#Requires -Version 5.1
#Requires -RunAsAdministrator
[CmdletBinding(SupportsShouldProcess)]
param(
	[ValidateSet('Register', 'Unregister')]
	[string]$Action = 'Register',

	[string]$LibraryPath = (Join-Path $PSScriptRoot 'out\Win32\Release\bin\AtkinsRealis.ProjectWise.QuickAccess.dll')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (Get-Process -Name pwc -ErrorAction SilentlyContinue) {
	throw 'Close ProjectWise Explorer before registering or unregistering Quick Access. No processes were stopped.'
}

$subkey = 'SOFTWARE\Bentley\ProjectWise\CustomModules\ThirdDigital.ProjectWise.QuickAccess'
$registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry32)
try {
	if ($Action -eq 'Register') {
		$library = (Resolve-Path -LiteralPath $LibraryPath).Path
		$stream = [IO.File]::OpenRead($library)
		$reader = [IO.BinaryReader]::new($stream)
		try {
			if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'The module is not a Windows executable.' }
			$stream.Position = 0x3C
			$stream.Position = $reader.ReadInt32()
			if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x14C) {
				throw 'ProjectWise Explorer 23.06 requires an x86 module.'
			}
		} finally { $reader.Dispose() }

		$pw = $registry.OpenSubKey('SOFTWARE\Bentley\ProjectWise')
		if ($null -eq $pw) { throw 'The 32-bit ProjectWise installation registry key was not found.' }
		try { $root = [string]$pw.GetValue('Path') } finally { $pw.Dispose() }
		if ([string]::IsNullOrWhiteSpace($root)) { throw 'The ProjectWise installation Path value is missing.' }
		$explorer = Join-Path $root 'bin\pwc.exe'
		$versionInfo = (Get-Item -LiteralPath $explorer).VersionInfo
		$version = [version]::new($versionInfo.FileMajorPart, $versionInfo.FileMinorPart, $versionInfo.FileBuildPart, $versionInfo.FilePrivatePart)
		if ($version -lt [version]'23.6.3.12' -or $version -ge [version]'23.6.4.0') {
			throw "This prototype targets Explorer 23.06.03.12 and later 23.06.03 builds; found $version."
		}
		if ($PSCmdlet.ShouldProcess("HKLM (32-bit)\$subkey", "Register $library")) {
			$key = $registry.CreateSubKey($subkey)
			try {
				$key.SetValue('Library', $library, [Microsoft.Win32.RegistryValueKind]::String)
				$key.SetValue('Function', 'CustomInitialize', [Microsoft.Win32.RegistryValueKind]::String)
				$key.SetValue('Application Mask', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
			} finally { $key.Dispose() }
			Write-Output "Registered Quick Access: $library"
			Write-Output 'Reopen Explorer. Use View > Ribbon or Tools > Quick Access Toolbar to show or hide the ribbon.'
		}
	}
	elseif ($PSCmdlet.ShouldProcess("HKLM (32-bit)\$subkey", 'Unregister Quick Access only')) {
		$registry.DeleteSubKeyTree($subkey, $false)
		Write-Output 'Quick Access registration removed. The DLL and Themes registration were not changed.'
	}
} finally { $registry.Dispose() }
