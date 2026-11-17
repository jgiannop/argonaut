; The Windows installer (Inno Setup): the folder the build puts together
; (argonaut.exe, its extensions, Qt's libraries) into Program Files, with a
; Start menu entry and an uninstaller.
;
;   iscc /DAppVersion=0.14.0 /DSource=..\dist installer\argonaut.iss
;
; Without administrator rights it installs for the current user only.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Source
  #define Source "..\dist"
#endif

[Setup]
AppId={{6E0B2C1E-5B1D-4C0A-9A53-A7C0F1E6D2B4}
AppName=Argonaut
AppVersion={#AppVersion}
AppPublisher=johngiannop
AppPublisherURL=https://github.com/jgiannop/argonaut
AppSupportURL=https://github.com/jgiannop/argonaut/issues
DefaultDirName={autopf}\Argonaut
DefaultGroupName=Argonaut
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
SetupIconFile=..\data\argonaut.ico
UninstallDisplayIcon={app}\argonaut.exe
OutputDir=..\installer-out
OutputBaseFilename=argonaut-{#AppVersion}-windows-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#Source}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\Argonaut"; Filename: "{app}\argonaut.exe"
Name: "{autodesktop}\Argonaut"; Filename: "{app}\argonaut.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\argonaut.exe"; Description: "{cm:LaunchProgram,Argonaut}"; Flags: nowait postinstall skipifsilent
