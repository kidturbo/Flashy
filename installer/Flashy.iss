; Flashy.iss — Inno Setup script for the Flashy J2534 Pass-Thru tools
;
; Build prerequisites:
;   1. Install Inno Setup 6: https://jrsoftware.org/isinfo.php
;   2. Run `python tools/build_exe.py` (without --zip) to populate
;      dist/Flashy-Tool/ with the firmware + tools
;   3. Build the installer:
;        "C:/Program Files (x86)/Inno Setup 6/ISCC.exe" installer/Flashy.iss
;      or use `python tools/build_exe.py --installer` to do it all in one step.
;
; Output: dist/Flashy-Tool-Setup-<version>.exe

#define AppName        "Flashy"
#define AppVersion     "1.5.1"
#define AppPublisher   "kidturbo"
#define AppURL         "https://github.com/kidturbo/Flashy"
#define AppExeFolder   "..\dist\Flashy-Tool"

[Setup]
; AppId is a stable GUID; change only if you fork the project to a
; new identity. Inno Setup uses this to detect/upgrade prior installs.
AppId={{6F2F5A82-7B41-4E2B-9C8F-FLASHY-J2534}}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases

; Install per-user (no UAC prompt). Default to Local AppData so the
; user can write to reads/ and writes/ without admin rights.
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UsePreviousAppDir=yes

; Output
OutputDir=..\dist
OutputBaseFilename=Flashy-Tool-Setup-{#AppVersion}
SetupIconFile=
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern

; License + readme shown during install (uncomment if you add files)
; LicenseFile=..\LICENSE
; InfoBeforeFile=..\dist\Flashy-Tool\README.md

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut for Flashy"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
; Top-level files
Source: "{#AppExeFolder}\firmware.uf2";              DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppExeFolder}\README.md";                 DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppExeFolder}\Read-Write-Instructions.html"; DestDir: "{app}"; Flags: ignoreversion

; Tool exes
Source: "{#AppExeFolder}\detect_port.exe";  DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppExeFolder}\vin_scan.exe";     DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppExeFolder}\vin_update.exe";   DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppExeFolder}\capture_bus.exe";  DestDir: "{app}"; Flags: ignoreversion

; Batch launchers
Source: "{#AppExeFolder}\*.bat";            DestDir: "{app}"; Flags: ignoreversion

; reads/ and writes/ scaffold folders (with their READMEs)
Source: "{#AppExeFolder}\reads\README.md";  DestDir: "{app}\reads"; Flags: ignoreversion
Source: "{#AppExeFolder}\writes\README.md"; DestDir: "{app}\writes"; Flags: ignoreversion

[Icons]
; Start Menu group — primary shortcut is the interactive menu;
; individual launchers are also exposed for power users.
Name: "{group}\{#AppName}";        Filename: "{app}\Flashy_Menu.bat";     WorkingDir: "{app}"; Comment: "Pick a tool from the Flashy menu"
Name: "{group}\Flash Firmware";    Filename: "{app}\Flash_Firmware.bat";  WorkingDir: "{app}"
Name: "{group}\Detect Port";       Filename: "{app}\Detect_Port.bat";     WorkingDir: "{app}"
Name: "{group}\VIN Scan";          Filename: "{app}\VIN_Scan.bat";        WorkingDir: "{app}"
Name: "{group}\VIN Update";        Filename: "{app}\VIN_Update.bat";      WorkingDir: "{app}"
Name: "{group}\Capture CAN Bus";   Filename: "{app}\Capture_CAN_Bus.bat"; WorkingDir: "{app}"
Name: "{group}\Read-Write Instructions"; Filename: "{app}\Read-Write-Instructions.html"
Name: "{group}\Open Install Folder";     Filename: "{app}"
Name: "{group}\Uninstall {#AppName}";    Filename: "{uninstallexe}"

; Optional desktop icon (gated by [Tasks] checkbox) — points at the
; interactive menu so the user picks the tool from there.
Name: "{userdesktop}\{#AppName}";  Filename: "{app}\Flashy_Menu.bat"; WorkingDir: "{app}"; Tasks: desktopicon; Comment: "Pick a tool from the Flashy menu"

[Run]
; Offer to open the install folder when setup finishes.
Filename: "{app}"; Description: "Open install folder"; Flags: postinstall shellexec skipifsilent unchecked

[UninstallDelete]
; Don't delete the user's reads/ or writes/ contents on uninstall —
; those are user data they may want to keep. Only the README we
; installed will be removed by the normal [Files] uninstaller.
; Add explicit Type: files entries here if you ever ship more
; uninstallable items.
