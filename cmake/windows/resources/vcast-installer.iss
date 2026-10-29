; Vcast Windows installer (Inno Setup 6)
; Built in CI by .github/scripts/Package-Windows.ps1:
;   ISCC.exe /DAppVersion=0.2.0 /DSourceDir=<release\Config\vcast> /DOutputDir=<release> /DOutputName=<name> vcast-installer.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{6E1C2B4A-8D3F-4F0B-9C55-7A1E2D9B3C40}
AppName=Vcast for OBS Studio
AppVersion={#AppVersion}
AppPublisher=Vcast contributors
AppPublisherURL=https://github.com/abahvictor360-sketch/Vcast
; OBS Studio 28+ loads plugins from this machine-wide folder.
DefaultDirName={commonappdata}\obs-studio\plugins\vcast
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Vcast for OBS Studio
LicenseFile={#SourceDir}\..\..\..\LICENSE
SetupLogging=yes

[Messages]
WelcomeLabel2=This will install Vcast [ver] into OBS Studio.%n%nVcast lets iPhones, Android phones and laptops (including Win + K) cast their screen into OBS, and casts OBS to smart TVs on your network.%n%nPlease close OBS Studio before continuing.

[Tasks]
Name: "wirelessdisplay"; Description: "Enable Win+K casting into OBS (installs Windows' Wireless Display feature; needs internet, can take a few minutes)"

[Files]
Source: "{#SourceDir}\bin\64bit\*.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "{#SourceDir}\bin\64bit\*.pdb"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs

[Run]
; Let phones and laptops on the local network reach OBS (AirPlay, cast page, Vcast app). Many home
; Wi-Fi networks are classified as "Public" by Windows, so the rule covers every profile; it only
; applies to obs64.exe.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Vcast for OBS Studio"""; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Vcast for OBS Studio"" dir=in action=allow program=""{code:ObsExe}"" enable=yes profile=any"; Flags: runhidden; StatusMsg: "Allowing OBS through Windows Firewall..."
; Windows' own Miracast receiver, which the "Vcast Win+K Receiver" source shows inside OBS.
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -Command ""Add-WindowsCapability -Online -Name 'App.WirelessDisplay.Connect~~~~0.0.1.0' | Out-Null"""; Flags: runhidden waituntilterminated; Tasks: wirelessdisplay; StatusMsg: "Installing Windows Wireless Display for Win+K casting (this can take a few minutes)..."

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Vcast for OBS Studio"""; Flags: runhidden; RunOnceId: "VcastFirewall"

[Code]
function ObsExe(Param: String): String;
var
  Dir: String;
begin
  if not RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Dir) then
    Dir := ExpandConstant('{commonpf64}\obs-studio');
  Result := AddBackslash(Dir) + 'bin\64bit\obs64.exe';
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not FileExists(ObsExe('')) then
    Result := MsgBox('OBS Studio was not found in the usual place. Vcast will still be installed for OBS Studio 28 or newer.' + #13#10#13#10 + 'Continue?', mbConfirmation, MB_YESNO) = IDYES;
end;
