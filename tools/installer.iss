; Inno Setup script for Sprout (PvZ2 PC Native Port)
; Run with: ISCC.exe tools\installer.iss

#define MyAppName "Sprout"
#define MyAppVersion "1.0"
#define MyAppPublisher "virgenes"
#define MyAppURL "https://github.com/virgenes/sprout"

[Setup]
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={autopf}\Sprout
DefaultGroupName=Sprout
OutputDir=..\dist
OutputBaseFilename=Sprout-Setup-{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
UninstallDisplayIcon={app}\launcher.exe
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Files]
Source: "..\launcher.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\sprout.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\config.ini"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\lib\*"; DestDir: "{app}\lib"; Flags: ignoreversion recursesubdirs
Source: "..\mesa\*"; DestDir: "{app}\mesa"; Flags: ignoreversion recursesubdirs
Source: "..\save\*"; DestDir: "{app}\save"; Flags: ignoreversion recursesubdirs; Check: DirExists(ExpandConstant('{src}\save'))

[Icons]
Name: "{group}\Sprout"; Filename: "{app}\launcher.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall Sprout"; Filename: "{uninstallexe}"
Name: "{commondesktop}\Sprout"; Filename: "{app}\launcher.exe"; WorkingDir: "{app}"

[Run]
Filename: "{app}\launcher.exe"; Description: "Launch Sprout"; Flags: postinstall nowait skipifsilent

[Code]
function DirExists(const Dir: string): Boolean;
begin
  Result := DirExists(Dir);
end;

function InitializeSetup: Boolean;
begin
  Result := True;
end;