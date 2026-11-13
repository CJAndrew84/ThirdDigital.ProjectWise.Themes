# ProjectWise Quick Access (prototype)

A separate solution and custom-module DLL for ProjectWise Explorer 23.06.03.12 (x86), independent of AtkinsRealis.ProjectWise.Themes. Neither the Themes solution nor its installer needs this module. The two modules can be registered together.

## Tabbed ribbon

- An **AtkinsRealis - Quick Access** toolbar that starts docked above Explorer's content pane. Right-click it and choose **Float** or **Dock at top**. Drag the docked left-hand grip to float it; double-click the floating title bar or drag it to the top of Explorer to dock again. This is a separate top docking row, not a replacement for Bentley's native toolbar/ribbon docking system.
- **Home:** New Folder, Add Documents, Check Out, Check In, Free, Properties, Refresh, Open and New Document.
- **View:** Thumbnails, Large Icons, Small Icons, List, Details, Manage Views and Interface.
- **Search:** Advanced Search, Find in Folder, a query field and Start Search. Press Enter in the query field to run ProjectWise Simple Search in the active document list. This uses the SDK's query-string API, not SQL.
- **Tools:** Local Documents (Local Document Organizer), User Settings, Wizard Manager and Help Index.
- The command panel is a **child page inside the native tab control**, not a toolbar beside a tabs-only strip. A dedicated **navigation row beneath it** contains Back, Forward, Up and the SDK's editable ProjectWise address control. This row stays visible when commands are minimized or the ribbon is hidden. In minimized mode, temporary commands open below the navigation row without moving it.
- The address control synchronizes with Explorer's tree and active document list and follows its current location without overwriting an address being edited. Explorer's original navigation toolbar and main-address registration are left intact; existing native toolbars and third-party commands, including Transmittals, remain available.
- **Back/Forward** use the SDK document-list navigator and its history count/current position. **Up** selects the selected tree item's parent with the SDK's DSC-tree selection function. Navigation is independent of the document command menus and address-bar synchronization: an address refresh failure can disable the address field without disabling healthy navigation buttons. Back/Forward correctly disable at the beginning/end of history, and Up disables at a tree root. A main-document-list fallback handles ribbon focus when there is no active list. Debug output records changes to numeric history position/count and button availability without recording locations or document names.
- Original 32 logical-pixel line icons with rounded strokes: stacked pages for Add Documents, opposite document arrows for Check Out/In, an open lock for Free, complete circular arrows for Refresh, and distinct layout/configuration symbols. Readable labels, flat tabs with an active underline, grouped command captions and hover highlights remain. Checked commands have an underline as well as their native accessible checked state. Commands remain 64 logical pixels high; typography starts at 11 pt and respects larger system message fonts. Colours follow Windows system colours, including the active Themes palette. Enabled and disabled icons retain at least 3:1 contrast without copying Bentley artwork.
- Buttons are disabled if their SDK-defined command is absent or disabled in Explorer's SDK-initialised menus. The ProjectWise menu layouts are loaded before their state is initialized. Select a folder in the tree or documents in the active document list first. Some buttons will not apply to a particular selection.
- Availability updates every half-second while visible and is checked again on clicking. The selection context is retained while using tabs and the query field. An Explorer modal dialog disables actions. Only commands on the selected tab can be dispatched. Narrow docked windows provide horizontal scrolling for commands that do not fit.
- Commands use the SDK's published identifiers and Explorer's normal `WM_COMMAND` routing, rather than performing check-in/out or document operations directly. The last focused selection control is retained while using the toolbar and restored before synchronous command dispatch. **Add Documents** maps to the Create Multiple Documents command and its normal wizard.
- The ribbon opens expanded each time Explorer starts. **Maximise** pins the tabs and full-size command panel; **Minimise** leaves tabs and navigation; **Hide** leaves navigation only. These controls affect the ribbon, not the Explorer window.
- Clicking a minimized tab temporarily opens its commands over the content pane. Choosing a command, clicking outside, pressing Escape or leaving Explorer dismisses them without permanently expanding the ribbon. Double-clicking a tab switches between expanded and minimized modes.
- Use **View > Ribbon** or **Tools > Quick Access Toolbar** to restore a hidden ribbon to its previous expanded/minimized mode. The floating window's close button also hides the ribbon. Dock position, tab and display mode are retained during the session only, not saved across Explorer restarts.

This is a custom tabbed command bar, not Microsoft's Ribbon framework or a replacement for Explorer's native toolbar docking system. It does not change Themes settings. The native SDK address control and search input retain their Windows/ProjectWise rendering.

## Accessibility

The ribbon implements **WCAG 2.2 AA-aligned desktop accessibility features**, not a declaration of full WCAG conformance for ProjectWise or its SDK controls:

- **Keyboard:** Alt+R restores/focuses the tabs. Left/Right Arrow and Home/End select tabs; Down Arrow or Enter opens commands. Tab/Shift+Tab traverses visible, enabled controls; commands form a single toolbar stop with Left/Right Arrow, Home/End and Enter/Space activation. Enter also activates Maximise, Minimise, Hide, Back, Forward, Up and Start Search without relying on the host's dialog manager. Space uses native button handling. Commands and search controls selected by keyboard scroll into view. F6 cycles between Explorer content, tabs and navigation; Shift+F6 reverses this. Escape dismisses temporary commands and returns focus to tabs, or returns to Explorer content when no popup is open. Ctrl+F1 switches expanded/minimized mode. Shift+F10 or the Menu key opens docking/display commands at the focused control. Ctrl+F6, AltGr and native address/editing keys remain available to Explorer. Modal Explorer dialogs block ribbon shortcuts.
- **Focus:** solid, DPI-scaled focus outlines use at least 3:1 contrast. Pointer hover/exit cannot change the keyboard command target or erase its outline. Availability refresh moves focus from a newly disabled command to an enabled command or the tabs. Collapse/hide and popup dismissal move focus to a visible control; Tab and F6 can leave the ribbon. The floating ribbon accepts activation and keyboard focus, retaining Explorer's selection context before it does so. Held activation keys are consumed until release to prevent repeated actions after focus changes.
- **Screen readers:** native tabs, toolbar buttons, edit/combo controls and buttons preserve their standard accessible roles/states. Windows accessibility annotations supply explicit location, search, ribbon-action and page names/descriptions; page names and selection/state notifications update with the UI. Icon-only navigation buttons retain accessible Back/Forward/Up names. These native MSAA providers are available to UI Automation through Windows' accessibility bridge.
- **Contrast:** custom-painted text enforces at least 4.5:1; enabled symbols, active-tab indicators and focus outlines enforce at least 3:1. Unreadable palette colours fall back to black/white without changing the saved theme. Disabled commands also expose their native disabled state, not just a colour difference. Windows contrast-theme colours and live theme changes are respected.
- **Targets and scaling:** mode/navigation buttons and tabs have at least 24 logical-pixel targets; command buttons are 112 by 64 logical pixels. The native address selection field is requested at 24 logical pixels high. Layout and drawing scale with window DPI. There are no animations or automatic document actions.

Automated native checks cover contrast reference values/thresholds, accessible names, measured targets, real queued keyboard routing, focus traversal/return, narrow-window search visibility, held-key dispatch and icon silhouettes. GitHub Actions builds and runs this separate check executable without the proprietary SDK. **Narrator/NVDA, native SDK address accessibility, Campbell/light/Windows contrast themes, 200% display scaling and real Explorer focus/command routing still require manual acceptance.** WCAG is a web-content standard; use [WCAG2ICT](https://www.w3.org/TR/wcag2ict-22/) when applying its criteria to this desktop software. See the [review and acceptance matrix](ACCESSIBILITY.md); an end-to-end assessment must also cover the host, native dialogs and third-party controls.

## Build

Use Visual Studio 2022 or newer with the v143 Desktop C++ tools, a Windows 10/11 SDK, and the separately licensed ProjectWise SDK with Win32 libraries. No MFC, ATL, MinHook or .NET dependency is added by Quick Access. The x86 Visual C++ runtime matching the compiler is required on the target machine.

Open `AtkinsRealis.ProjectWise.QuickAccess.sln` in the repository root, not the Themes solution. From a Developer PowerShell in the repository root:

```powershell
$env:PWSDKDir = 'C:\Program Files\Bentley\ProjectWise\SDK'
msbuild .\AtkinsRealis.ProjectWise.QuickAccess.sln -t:Build -p:Configuration=Release -p:Platform=x86
.\ThirdDigital.ProjectWise.QuickAccess\Checks\out\Win32\Release\bin\AtkinsRealis.ProjectWise.QuickAccess.Checks.exe
```

DLL: `ThirdDigital.ProjectWise.QuickAccess\out\Win32\Release\bin\AtkinsRealis.ProjectWise.QuickAccess.dll`.

The native checks use a fake host and real Windows controls. They verify labels, dimensions, actual tab-page ownership and containment, all four tabs, inactive-command gating, nested native tab/button notifications, expanded/minimized/hidden transitions, temporary-popup dismissal, persistent navigation layout/dispatch, history-boundary availability, independent navigation/address failures, accessibility features, query submission, selection-context retention, docking, scrolling, content resizing/restoration and safe shutdown without accessing a datasource or changing documents. They do **not** prove Explorer's SDK menu initialization, navigator/history integration, parent selection, native address synchronization, search execution or command routing works for every selection. Test the prototype against a disposable datasource before using commands on production documents, particularly **Free**, which invokes Explorer's normal operation and may discard local changes.

## Register for local development

There is no Quick Access MSI in this first version. Build the Release DLL, close Explorer, then run the following in an **administrator PowerShell**, from the repository root:

```powershell
.\ThirdDigital.ProjectWise.QuickAccess\Register-Module.ps1 -Action Register
```

The script registers the full build-output path under the **32-bit** registry view:

`HKLM\SOFTWARE\WOW6432Node\Bentley\ProjectWise\CustomModules\ThirdDigital.ProjectWise.QuickAccess`

It checks for Explorer 23.06.03.12 or a later 23.06.03 build and an x86 DLL. It does not copy files, edit Bentley files, change the Themes registration, close applications or install anything else. Keep the DLL at the registered location. To use a stable deployment folder, copy the DLL there yourself and pass `-LibraryPath 'C:\YourFolder\AtkinsRealis.ProjectWise.QuickAccess.dll'`. `-WhatIf` previews registration without writing registry values.

Restart Explorer. The toolbar should appear shortly after its main window is ready. Its DLL remains loaded until Explorer exits; close Explorer before rebuilding or changing registration. Diagnostics use `OutputDebugString` with the prefix `ProjectWise Quick Access`.

## Remove

Close Explorer and run in administrator PowerShell:

```powershell
.\ThirdDigital.ProjectWise.QuickAccess\Register-Module.ps1 -Action Unregister
```

Only this module's registry key is removed. Themes, existing Explorer toolbars and the DLL file are left untouched.

## Explorer acceptance checks

On a disposable datasource, confirm startup docked above the content pane, right-click **Float** / **Dock at top**, dragging the grip to float, title-bar double-click to dock, frame resizing without overlapping content, moving the ribbon between monitors, and closing Explorer. Exercise Home, View, Search and Tools. Verify commands are inside the tab page, **Maximise**, **Minimise**, temporary tab opening, Escape/outside-click dismissal, **Hide**, **View > Ribbon** restoration and the floating close button. Confirm the navigation row beneath the tab section remains visible and usable in every display mode. Exercise Back/Forward/Up, a typed address, the address drop-down, tree/list changes and datasource changes; address text being edited must not be overwritten. On narrow windows, scroll and keyboard-navigate to the final command.

Select a folder, one document, several documents and no document; compare each button's enabled state with Explorer's context menu. Confirm each enabled button opens the intended standard operation, cancellation works normally, and no document action or search can run behind a modal dialog. Exercise simple search using both Enter and Start Search, plus Advanced Search and Find in Folder. Run the keyboard sequence above without a mouse and with Narrator or NVDA: verify names, tab selection, command enabled/disabled states, focused-item visibility, hide/restore and returning to Explorer. Repeat with Themes unregistered, Campbell applied, a Windows contrast theme and 200% display scaling. Real Explorer integration, assistive-technology compatibility and mixed-DPI behaviour require this manual acceptance pass; no such pass is implied by the isolated native checks.
