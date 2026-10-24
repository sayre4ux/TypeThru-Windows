; Inno Setup script for TypeThru. Build with scripts\package.cmd (finds ISCC),
; or directly:  ISCC installer\typethru.iss
#define Version ReadIni(SourcePath + "\..\installer\version.ini", "TypeThru", "Version", "0.1.0")

[Setup]
AppId={{8E9C4D21-7B6A-4E5F-9C3D-1A2B3C4D5E6F}
AppName=TypeThru
AppVersion={#Version}
AppPublisher=TypeThru
AppPublisherURL=https://github.com/sayre4ux/TypeThru-Windows
DefaultDirName={autopf}\TypeThru
DefaultGroupName=TypeThru
UninstallDisplayIcon={app}\TypeThru.exe
OutputDir=..\installer
OutputBaseFilename=TypeThru-Setup-{#Version}
Compression=lzma2/max
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
MinVersion=10.0.19044
WizardStyle=modern
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
SetupIconFile=..\res\app.ico

[Files]
Source: "..\.build\TypeThru.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\TypeThru"; Filename: "{app}\TypeThru.exe"
Name: "{autodesktop}\TypeThru"; Filename: "{app}\TypeThru.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; Flags: unchecked
Name: "runatlogin"; Description: "Open TypeThru when I sign in"; Flags: unchecked

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "TypeThru"; ValueData: """{app}\TypeThru.exe"""; Flags: uninsdeletevalue; Tasks: runatlogin

[Run]
Filename: "{app}\TypeThru.exe"; Description: "Open TypeThru"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; The app also removes this entry itself; the uninstall run covers setups where it is closed.
Filename: "{cmd}"; Parameters: "/C reg delete HKCU\Software\TypeThru /f"; Flags: runhidden; RunOnceId: "RemoveSettings"

[UninstallDelete]
; Nothing in {app} is user data; settings live in the registry only.
