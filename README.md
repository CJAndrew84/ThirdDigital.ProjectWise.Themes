# AtkinsRealis.ProjectWise.Themes

Terminal-inspired colour themes for Bentley ProjectWise Explorer, based on the standard Windows Terminal colour schemes.

Loaded by ProjectWise Explorer as a custom module. Not affiliated with or endorsed by Bentley Systems.

> **Work in progress — compatibility is not guaranteed.** Application testing has only covered **Windows 11 Pro 26H2 (OS build 26300.9457)** with **ProjectWise Explorer CONNECT Edition 10.00.03.434 (32-bit)**. The installer built from this source also accepts 10.00.03 builds including .453 and 32-bit Explorer 23.06.03.12 through later 23.06.03 builds, but those builds have not been tested in Explorer. On the inspected 23.06.03.12 installation, `pwc.exe` is x86 even though x64 ProjectWise libraries are also installed.

A **custom theme editor is planned for a future release**. The current version provides preset themes only.

### Separate Quick Access toolbar prototype

[ProjectWise Quick Access](ThirdDigital.ProjectWise.QuickAccess/README.md) is an independent companion solution with a dockable, tabbed command bar (Home, View, Search and Tools), contained command pages, Maximise/Minimise/Hide modes and a persistent native navigation/address row underneath. Its theme-aware styling includes WCAG 2.2 AA-aligned keyboard, focus, contrast and screen-reader features; full host accessibility conformance is not claimed. Open `AtkinsRealis.ProjectWise.QuickAccess.sln`; its build, registration and removal instructions are separate from Themes. It is not included in the Themes installer.

## Dark theme previews

Actual Explorer captures with private datasource, project, document and account details covered by opaque redactions. These show the presets, not a custom theme editor. See the [screenshot gallery](assets/screenshots/README.md) for sharing notes and the tested environment.

| One Half Dark | Solarized Dark | Campbell Powershell |
| --- | --- | --- |
| [![One Half Dark](assets/screenshots/one-half-dark.png)](assets/screenshots/one-half-dark.png) | [![Solarized Dark](assets/screenshots/solarized-dark.png)](assets/screenshots/solarized-dark.png) | [![Campbell Powershell](assets/screenshots/campbell-powershell.png)](assets/screenshots/campbell-powershell.png) |

## Building

Requirements:

- Visual Studio 2022 or newer with the "Desktop development with C++" workload and v143 toolset.
- The Bentley ProjectWise SDK, which Bentley provides (10.00.03 or 23.06 for the respective Explorer version). It is not included in this repository.

Clone with the MinHook submodule:

```
git clone --recursive <repository URL>
```

Point `PWSDKDir` at the SDK root (the folder containing `include\aawindms.h` and `lib\Win32\dmawin.lib`) for the build process only, then build Release x86 from a Developer PowerShell for Visual Studio 2022 or newer:

```powershell
$env:PWSDKDir = 'C:\path\to\ProjectWise SDK'
msbuild AtkinsRealis.ProjectWise.Themes.sln -t:AtkinsRealis_ProjectWise_Themes -p:Configuration=Release -p:Platform=x86
```

This builds only the module and the MinHook library it uses. Building the full solution, including the tests and harnesses, also needs the .NET Framework 4.6.2 targeting pack.

The module is written to:

```
ThirdDigital.ProjectWise.Themes\out\Win32\Release\bin\AtkinsRealis.ProjectWise.Themes.dll
```

The build stops with an error naming `PWSDKDir` if the SDK cannot be found.

`AtkinsRealis.ProjectWise.Themes.Harness\Verify.ps1` builds every project and runs the unit tests and the native and WinForms harnesses, for example `.\AtkinsRealis.ProjectWise.Themes.Harness\Verify.ps1 -Suite Full -Configuration Release -ResultsRoot $env:TEMP\pw-themes -PWSDKDir 'C:\path\to\ProjectWise SDK'`.

## Installing

Download the x86 MSI from [Releases](https://github.com/Third-Digital/ThirdDigital.ProjectWise.Themes/releases), close ProjectWise Explorer and run the installer as an administrator. Reopen Explorer and choose **Tools > Colour Theme**. Existing GitHub releases may not include the 23.06 installer changes; for those changes, build the MSI from this source using `build\Build-Release.ps1 -SkipTests` (for local development) or the release procedure in [RELEASING.md](RELEASING.md). If an earlier package with the same version is already installed, uninstall it before installing a development build; Windows Installer requires a higher version for an upgrade.

Requires the [Microsoft Visual C++ v14 Redistributable (x86)](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist), version 14.44.35207 or newer. The installer checks both this runtime and the supported ProjectWise version. The installer is unsigned.

The installer built from this source puts the module under `Program Files (x86)\AtkinsRealis\ProjectWise Themes` and registers it with ProjectWise. It retains the legacy `10.00\CustomModules` registration and additionally registers under `CustomModules` for detected 23.06.03 Explorer. It has not yet been validated in 23.06 Explorer. Theme preferences are stored per user. It does not modify Bentley's application files.

For manual development installation, copy the DLL to a folder of your choice, adjust the `Library` value below to match, and import the file as an administrator:

```reg
Windows Registry Editor Version 5.00

[HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Bentley\ProjectWise\10.00\CustomModules\ThirdDigital.ProjectWise.Themes]
"Application Mask"=dword:00000001
"Function"="CustomInitialize"
"Library"="C:\\ProjectWiseThemes\\AtkinsRealis.ProjectWise.Themes.dll"
```

For 23.06.03, also register the same values under `HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Bentley\ProjectWise\CustomModules\ThirdDigital.ProjectWise.Themes`. Restart ProjectWise Explorer.

## Using

Open **Tools > Colour Theme** and choose one of:

- Campbell
- Campbell Powershell
- Vintage
- One Half Dark
- One Half Light
- Solarized Dark
- Solarized Light
- Tango Dark
- Tango Light
- None

The change applies straight away to the open windows. The choice is saved per user under `HKEY_CURRENT_USER\Software\Third Digital\ProjectWise Themes` (value `Scheme`) and used the next time Explorer starts. **None** returns Explorer to its normal appearance and deletes the saved value.

## Uninstalling

Close ProjectWise Explorer, then uninstall **AtkinsRealis ProjectWise Themes** from Windows Installed apps. Repair is also available by running the MSI again. Saved theme preferences remain in your user profile.

For a manual installation, delete the applicable `CustomModules\ThirdDigital.ProjectWise.Themes` keys described above and remove the DLL after Explorer exits. If the MSI replaced a manual registration, uninstall removes that registration; it does not restore the old development path.

## Coverage

The module changes colours only, and only inside ProjectWise Explorer's own process.

- The menu bar follows the scheme.
- Toolbars, docking panes and MFC popup menus follow the scheme. Some accent colours that Explorer takes from the Windows theme stay as they are.
- Work Area Properties follows the scheme, including its cached white and grey grid fills. Drawing colours are remapped while supported native controls paint; cached brushes, bitmaps and coloured icons keep their original values.
- The folder tree and document list follow the scheme; native tabs also follow the scheme. Rich edit fields are partly themed. Under dark schemes their scrollbars, headers and glyphs use Windows' dark theme.
- Native dialog pages, labels, edit boxes, list boxes and combo boxes follow the scheme, including lazily created Document Properties pages. Under dark schemes, native buttons and checkbox glyphs use Windows’ dark styling. Checkbox backing and Bentley’s simple attribute labels follow the palette without changing their state, text or font. Map previews and other bitmap content keep their original colours.
- The native Access Control pane follows the scheme, including alternating permission rows, toolbar backgrounds and dark checkbox glyphs. Permission values, checked/disabled states and coloured icons are preserved.
- The Audit Trail HTML report follows the palette through a screen-only stylesheet. None removes it and restores the original report styling. Other embedded web content is not changed.
- Title bars and window borders follow dark schemes on Windows 11 (build 22000 or later).
- Document windows inside Explorer (MDI child windows) have their captions and frames drawn in the scheme's colours. Minimised windows keep the standard Windows styling.
- Context menus use Windows' dark menus under dark schemes.
- Dependency Viewer follows the scheme, including its graph canvas, neutral node fills and outlines, text, and Office2007 panel/tab backgrounds. Drawing uses temporary palette brushes; graph data, semantic accents and bitmap icons remain unchanged. None restores the original colours.
- Other WinForms areas that use the standard system colours follow the scheme. Unrelated DotNetBar or fixed-colour surfaces are not themed.
- WPF windows and the embedded Chromium web panes are not themed.

## Third-party software

The preset palette data is adapted from [Microsoft Windows Terminal](https://github.com/microsoft/terminal/blob/1282d56a33bb15206fcb3a09d3bea0a33036f2cd/src/cascadia/TerminalSettingsModel/defaults.json) under the MIT licence. See [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt) for its source revision, Microsoft copyright and full licence, together with the MinHook notices. This file is included in new installer builds.

This module includes [MinHook](https://github.com/TsudaKageyu/minhook) v1.3.4, built from source as the `external/minhook` submodule, under the BSD 2-Clause licence. See [external/minhook/LICENSE.txt](external/minhook/LICENSE.txt) for the complete notice, including the Hacker Disassembler Engine notices.

## Issues and contributions

Found a problem or want to help? Use the [bug report or feature request forms](https://github.com/Third-Digital/ThirdDigital.ProjectWise.Themes/issues/new/choose), and read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request. Include exact versions and sanitised screenshots when reporting visual issues.

## Installer and releases

The WiX installer project is in `AtkinsRealis.ProjectWise.Themes.Install`. Run `build/Build-Release.ps1` to verify and package a release. See [RELEASING.md](RELEASING.md) for build, validation and publishing instructions.

## Licence

MIT. See [LICENSE](LICENSE).
