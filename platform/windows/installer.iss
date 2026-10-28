; Windows installer for a packaged game (Inno Setup 6). Used by .github/workflows/release.yml:
;   iscc /DAppName="Iron and Gold" /DAppVersion=0.1.0 /DAppId=com.rtsengine.ironandgold
;        /DExeName=Iron_and_Gold /DSourceDir=dist\windows /DOutputDir=out platform\windows\installer.iss
; Per-user install (no admin prompt), Start menu + optional desktop shortcut, clean uninstall.
#ifndef AppName
  #error Pass /DAppName=...
#endif
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef AppId
  #define AppId AppName
#endif
#ifndef SourceDir
  #define SourceDir "dist\windows"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename={#ExeName}-{#AppVersion}-setup
UninstallDisplayIcon={app}\{#ExeName}.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#ExeName}.exe"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#ExeName}.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\{#ExeName}.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
