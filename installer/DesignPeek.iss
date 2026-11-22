; DesignPeek installer (Inno Setup 6)
; Build: ISCC.exe /DAppVersion=0.1.0 /DAppSourceDir=<folder with published app> installer\DesignPeek.iss

#ifndef AppVersion
  #define AppVersion "0.1.0-dev"
#endif
#ifndef AppSourceDir
  #define AppSourceDir "..\artifacts\app"
#endif

#define AppName "DesignPeek"

[Setup]
AppId={{6F2A9C3E-1B7D-4E58-9A0C-3D5E7F812B46}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=DesignPeek contributors
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
LicenseFile=..\LICENSE
OutputDir=Output
OutputBaseFilename=DesignPeek-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ChangesAssociations=yes
UninstallDisplayName={#AppName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; The shim DLL may be loaded by Windows' thumbnail process during upgrade/uninstall:
; replace/delete it on next restart if it is in use.
Source: "{#AppSourceDir}\DesignPeek.Shim.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#AppSourceDir}\*"; Excludes: "DesignPeek.Shim.dll,*.pdb"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
const
  ThumbClsid = '{D4D3280A-4CA9-4771-BB01-B9336E54579E}';   { must match src/native/shim/Guids.h }
  ThumbHandlerIID = '{E357FCCD-A995-4576-B01F-234630154E96}'; { IThumbnailProvider }
  HandlerName = 'DesignPeek Thumbnail Provider';
  BackupKey = 'Software\DesignPeek\HandlerBackup';
  ApprovedKey = 'Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved';

procedure RegisterComServer();
var
  Key: String;
begin
  Key := 'Software\Classes\CLSID\' + ThumbClsid;
  RegWriteStringValue(HKLM, Key, '', HandlerName);
  RegWriteStringValue(HKLM, Key + '\InprocServer32', '', ExpandConstant('{app}\DesignPeek.Shim.dll'));
  RegWriteStringValue(HKLM, Key + '\InprocServer32', 'ThreadingModel', 'Apartment');
  RegWriteStringValue(HKLM, ApprovedKey, ThumbClsid, HandlerName);
end;

procedure UnregisterComServer();
begin
  RegDeleteKeyIncludingSubkeys(HKLM, 'Software\Classes\CLSID\' + ThumbClsid);
  RegDeleteValue(HKLM, ApprovedKey, ThumbClsid);
end;

function HandlerKey(Base, Ext: String): String;
begin
  Result := 'Software\Classes\' + Base + Ext + '\ShellEx\' + ThumbHandlerIID;
end;

{ Take over the thumbnail handler for one registry location, remembering whoever had it before. }
procedure ClaimHandler(Key: String);
var
  Previous: String;
begin
  if RegQueryStringValue(HKLM, Key, '', Previous) then
  begin
    if (CompareText(Previous, ThumbClsid) <> 0) and (not RegValueExists(HKLM, BackupKey, Key)) then
      RegWriteStringValue(HKLM, BackupKey, Key, Previous);
  end;
  RegWriteStringValue(HKLM, Key, '', ThumbClsid);
end;

{ Give the handler back to the previous owner, or remove ours if there was none. }
procedure ReleaseHandler(Key: String);
var
  Current, Previous: String;
begin
  if RegQueryStringValue(HKLM, Key, '', Current) and (CompareText(Current, ThumbClsid) = 0) then
  begin
    if RegQueryStringValue(HKLM, BackupKey, Key, Previous) then
      RegWriteStringValue(HKLM, Key, '', Previous)
    else
      RegDeleteKeyIncludingSubkeys(HKLM, Key);
  end;
end;

procedure UpdateExtensions(Claim: Boolean);
var
  Exts: TArrayOfString;
  I: Integer;
begin
  SetArrayLength(Exts, 2);
  Exts[0] := '.psd';
  Exts[1] := '.psb';
  for I := 0 to GetArrayLength(Exts) - 1 do
  begin
    if Claim then
    begin
      ClaimHandler(HandlerKey('', Exts[I]));
      ClaimHandler(HandlerKey('SystemFileAssociations\', Exts[I]));
    end
    else
    begin
      ReleaseHandler(HandlerKey('', Exts[I]));
      ReleaseHandler(HandlerKey('SystemFileAssociations\', Exts[I]));
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    RegisterComServer();
    UpdateExtensions(True);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    UpdateExtensions(False);
    UnregisterComServer();
    RegDeleteKeyIncludingSubkeys(HKLM, 'Software\DesignPeek');
  end;
end;
