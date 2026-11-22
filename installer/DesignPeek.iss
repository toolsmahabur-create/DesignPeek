; DesignPeek installer (Inno Setup 6) — per-user, no admin rights, one click.
; Build: ISCC.exe /DAppVersion=0.2.0 /DAppSourceDir=<folder with the 3 app files> installer\DesignPeek.iss

#ifndef AppVersion
  #define AppVersion "0.2.0-dev"
#endif
#ifndef AppSourceDir
  #define AppSourceDir "..\build\native\Release"
#endif

#define AppName "DesignPeek"

[Setup]
AppId={{0C3B5E8A-7D41-4F2B-9E6A-5A1D2C3B4E5F}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=DesignPeek contributors
AppPublisherURL=https://github.com/toolsmahabur-create/DesignPeek
AppSupportURL=https://github.com/toolsmahabur-create/DesignPeek/issues
PrivilegesRequired=lowest
DefaultDirName={autopf}\{#AppName}
UsePreviousAppDir=yes
DisableWelcomePage=yes
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir=Output
OutputBaseFilename=DesignPeek-Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
WizardSizePercent=100
ChangesAssociations=yes
CloseApplications=no
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\DesignPeek.exe
ShowLanguageDialog=auto

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "bangla"; MessagesFile: "compiler:Default.isl,Bangla.isl"

[CustomMessages]
english.LaunchApp=Start DesignPeek now (hover over PSD files in File Explorer)
bangla.LaunchApp=এখনই DesignPeek চালু করুন (File Explorer-এ PSD ফাইলের ওপর মাউস রাখুন)
english.OldVersionFound=An older DesignPeek installed for all users was found.%n%nRemove it now? (Windows will ask for permission once.)
bangla.OldVersionFound=আগের একটি DesignPeek (সব ইউজারের জন্য install করা) পাওয়া গেছে।%n%nএখনই সেটা মুছে ফেলব? (Windows একবার অনুমতি চাইবে।)

[Files]
Source: "{#AppSourceDir}\DesignPeek.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppSourceDir}\DesignPeek.Worker.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppSourceDir}\DesignPeek.Shim.dll"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "DesignPeek"; ValueData: """{app}\DesignPeek.exe"""; Flags: uninsdeletevalue

[Run]
Filename: "{app}\DesignPeek.exe"; Parameters: "--first-run"; Description: "{cm:LaunchApp}"; Flags: nowait postinstall

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
const
  ThumbClsid = '{D4D3280A-4CA9-4771-BB01-B9336E54579E}';   { must match src/native/shim/Guids.h }
  ThumbHandlerIID = '{E357FCCD-A995-4576-B01F-234630154E96}'; { IThumbnailProvider }
  HandlerName = 'DesignPeek Thumbnail Provider';
  BackupKey = 'Software\DesignPeek\HandlerBackup';
  ApprovedKey = 'Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved';
  OldMachineWideKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6F2A9C3E-1B7D-4E58-9A0C-3D5E7F812B46}_is1';
  AppWindowClass = 'DesignPeekApp';
  WmClose = $0010;

{ ---------- running app ---------- }

procedure CloseRunningApp();
var
  Window: HWND;
  I: Integer;
begin
  Window := FindWindowByClassName(AppWindowClass);
  if Window <> 0 then
  begin
    PostMessage(Window, WmClose, 0, 0);
    for I := 1 to 30 do
    begin
      Sleep(100);
      if FindWindowByClassName(AppWindowClass) = 0 then
        Break;
    end;
  end;
end;

{ Windows may still have the old shim DLL loaded in its thumbnail process. A loaded DLL cannot be
  overwritten or deleted, but it can be renamed out of the way. }
procedure MoveAsideShim(TargetFolder: String);
var
  Shim: String;
  FindRec: TFindRec;
begin
  Shim := ExpandConstant('{app}\DesignPeek.Shim.dll');
  if FileExists(Shim) and (not DeleteFile(Shim)) then
    RenameFile(Shim, AddBackslash(TargetFolder) + 'DesignPeek.Shim.old.' + GetDateTimeString('yyyymmddhhnnsszzz', #0, #0) + '.dll');

  if FindFirst(ExpandConstant('{app}\DesignPeek.Shim.old.*'), FindRec) then
  try
    repeat
      DeleteFile(ExpandConstant('{app}\') + FindRec.Name);
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
end;

{ ---------- older machine-wide (admin) version 0.1 ---------- }

function InitializeSetup(): Boolean;
var
  UninstallString: String;
  ResultCode: Integer;
begin
  Result := True;
  if RegQueryStringValue(HKLM64, OldMachineWideKey, 'UninstallString', UninstallString) then
  begin
    if MsgBox(CustomMessage('OldVersionFound'), mbConfirmation, MB_YESNO) = IDYES then
      ShellExec('', RemoveQuotes(UninstallString), '/SILENT /NORESTART', '', SW_SHOW, ewWaitUntilTerminated, ResultCode);
  end;
end;

{ ---------- COM + thumbnail handler registration (current user) ---------- }

procedure RegisterComServer();
var
  Key: String;
begin
  Key := 'Software\Classes\CLSID\' + ThumbClsid;
  RegWriteStringValue(HKCU, Key, '', HandlerName);
  RegWriteStringValue(HKCU, Key + '\InprocServer32', '', ExpandConstant('{app}\DesignPeek.Shim.dll'));
  RegWriteStringValue(HKCU, Key + '\InprocServer32', 'ThreadingModel', 'Apartment');
  RegWriteStringValue(HKCU, ApprovedKey, ThumbClsid, HandlerName);
end;

procedure UnregisterComServer();
begin
  RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\CLSID\' + ThumbClsid);
  RegDeleteValue(HKCU, ApprovedKey, ThumbClsid);
end;

function HandlerKey(Base, Ext: String): String;
begin
  Result := 'Software\Classes\' + Base + Ext + '\ShellEx\' + ThumbHandlerIID;
end;

{ Take over the thumbnail handler, remembering whoever had it before. }
procedure ClaimHandler(Key: String);
var
  Previous: String;
begin
  if RegQueryStringValue(HKCU, Key, '', Previous) then
  begin
    if (CompareText(Previous, ThumbClsid) <> 0) and (not RegValueExists(HKCU, BackupKey, Key)) then
      RegWriteStringValue(HKCU, BackupKey, Key, Previous);
  end;
  RegWriteStringValue(HKCU, Key, '', ThumbClsid);
end;

{ Give the handler back to the previous owner, or remove ours. }
procedure ReleaseHandler(Key: String);
var
  Current, Previous: String;
begin
  if RegQueryStringValue(HKCU, Key, '', Current) and (CompareText(Current, ThumbClsid) = 0) then
  begin
    if RegQueryStringValue(HKCU, BackupKey, Key, Previous) then
      RegWriteStringValue(HKCU, Key, '', Previous)
    else
      RegDeleteKeyIncludingSubkeys(HKCU, Key);
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

{ ---------- install / uninstall steps ---------- }

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  CloseRunningApp();
  MoveAsideShim(ExpandConstant('{app}'));
  Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    RegisterComServer();
    UpdateExtensions(True);
  end;
end;

function InitializeUninstall(): Boolean;
begin
  CloseRunningApp();
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    UpdateExtensions(False);
    UnregisterComServer();
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\DesignPeek');
    MoveAsideShim(GetTempDir());
  end;
end;
