; Compile through the CMake flaccd-installer target, using the audited payload.
#ifndef APP_VERSION
   #error APP_VERSION must be supplied by CMake
#endif
#ifndef PAYLOAD_DIR
   #error PAYLOAD_DIR must be supplied by CMake
#endif
#ifndef OUTPUT_DIR
   #error OUTPUT_DIR must be supplied by CMake
#endif
#ifndef APP_PUBLISHER
   #error APP_PUBLISHER must be supplied by CMake
#endif
#ifndef PRODUCT_NAME
   #error PRODUCT_NAME must be supplied by CMake
#endif
#ifndef MANIFEST_FILE
   #error MANIFEST_FILE must be supplied by CMake
#endif
#if VER < EncodeVer(6, 3, 0)
   #error Inno Setup 6.3 or newer is required
#endif

[Setup]
AppId={{70A8B7A2-BE27-4D88-9566-76E87C57F284}
AppName={#PRODUCT_NAME}
AppPublisher={#APP_PUBLISHER}
AppVersion={#APP_VERSION}
AppVerName=FlacCD {#APP_VERSION}
VersionInfoVersion={#APP_VERSION}
VersionInfoCompany={#APP_PUBLISHER}
VersionInfoProductName={#PRODUCT_NAME}
VersionInfoDescription={#PRODUCT_NAME} Setup
DefaultDirName={localappdata}\Programs\FlacCD
DefaultGroupName=FlacCD
DisableDirPage=yes
UsePreviousAppDir=yes
UninstallLogMode=append
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0
OutputDir={#OUTPUT_DIR}
OutputBaseFilename=FlacCD-{#APP_VERSION}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\flaccd.exe
ChangesAssociations=yes
CloseApplications=no
RestartApplications=no
SetupMutex=FlacCD.Setup.v1

[Files]
Source: "{#PAYLOAD_DIR}\*"; DestDir: "{app}"; Excludes: ".FlacCD-deployment,imports\*,source-imports\*"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MANIFEST_FILE}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\FlacCD installation and usage"; Filename: "{sys}\notepad.exe"; Parameters: """{app}\docs\WindowsInstaller.md"""

[Code]
const
   SHELL_KEY = 'Software\Classes\SystemFileAssociations\.flac\shell\FlacCD.MountAudioCD';
   UNINSTALL_KEY = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{70A8B7A2-BE27-4D88-9566-76E87C57F284}_is1';
   IMGDRIVE_KEY = 'Software\Microsoft\Windows\CurrentVersion\App Paths\ImgDrive.exe';

var
   registrationFailed: Boolean;
   oldFiles: TArrayOfString;

procedure removeObsoleteBinaries();
var
   newFiles: TArrayOfString;
   index, next, separator: Integer;
   name, digest, candidate: String;
   retained: Boolean;
begin
   if GetArrayLength(oldFiles) = 0 then begin
      Exit;
   end;
   if oldFiles[0] <> 'FlacCD installed binaries v1' then begin
      Log('Unknown old binary manifest; obsolete files retained.');
      Exit;
   end;
   if not LoadStringsFromFile(ExpandConstant('{app}\FlacCDFiles.txt'), newFiles) then begin
      Log('Cannot read new binary manifest; obsolete files retained.');
      Exit;
   end;
   for index := 1 to GetArrayLength(oldFiles) - 1 do begin
      separator := Pos('|', oldFiles[index]);
      if separator > 1 then begin
         name := Copy(oldFiles[index], 1, separator - 1);
         digest := Copy(oldFiles[index], separator + 1, 64);
         { Only top-level binaries may be obsolete. Never delete a directory,
           user cache, arbitrary path, or a modified installed file. }
         if (ExtractFileName(name) = name) and (Pos(':', name) = 0) and
            (Pos('/', name) = 0) and (Pos('\', name) = 0) and
            ((CompareText(ExtractFileExt(name), '.dll') = 0) or
             (CompareText(ExtractFileExt(name), '.exe') = 0)) then begin
            retained := False;
            for next := 1 to GetArrayLength(newFiles) - 1 do begin
               if CompareText(Copy(newFiles[next], 1, Pos('|', newFiles[next]) - 1), name) = 0 then begin
                  retained := True;
               end;
            end;
            candidate := ExpandConstant('{app}\') + name;
            if not retained and FileExists(candidate) then begin
               try
                  if CompareText(GetSHA256OfFile(candidate), digest) = 0 then begin
                     if not DeleteFile(candidate) then begin
                        Log('Obsolete binary is locked; retained: ' + candidate);
                     end else begin
                        Log('Removed unchanged obsolete binary: ' + candidate);
                     end;
                  end else begin
                     Log('Modified obsolete binary retained: ' + candidate);
                  end;
               except
                  Log('Cannot inspect obsolete binary; retained: ' + candidate);
               end;
            end;
         end;
      end;
   end;
end;

function expandEnvironment(source: String; destination: String; size: LongWord): LongWord;
   external 'ExpandEnvironmentStringsW@kernel32.dll stdcall';

function expandedPath(const text: String): String;
var
   buffer: String;
   size: LongWord;
begin
   SetLength(buffer, 32768);
   size := expandEnvironment(text, buffer, 32768);
   if (size > 0) and (size <= 32768) then begin
      SetLength(buffer, size - 1);
      Result := buffer;
   end else begin
      Result := '';
   end;
end;

function runFlacCD(const executable, parameters: String): String;
var
   resultCode: Integer;
begin
   Result := '';
   Log('FlacCD maintenance: ' + executable + ' ' + parameters);
   if not Exec(executable, parameters, ExtractFileDir(executable), SW_HIDE,
               ewWaitUntilTerminated, resultCode) then begin
      Result := 'Cannot launch FlacCD: ' + SysErrorMessage(resultCode);
   end else if resultCode <> 0 then begin
      Result := 'FlacCD command failed (' + IntToStr(resultCode) + '): ' + parameters +
                '. Run this command in a terminal for diagnostics. No forced termination was attempted.';
   end;
end;

function registeredImgDrive(const root: Integer): Boolean;
var
   executable: String;
begin
   Result := False;
   if RegQueryStringValue(root, IMGDRIVE_KEY, '', executable) then begin
      executable := RemoveQuotes(expandedPath(executable));
      Result := FileExists(executable);
   end;
end;

function imgDriveDetected(): Boolean;
begin
   Result := FileExists(GetEnv('FLACCD_IMGDRIVE')) or
             registeredImgDrive(HKCU64) or registeredImgDrive(HKCU32) or
             registeredImgDrive(HKLM64) or registeredImgDrive(HKLM32) or
             FileExists(ExpandConstant('{autopf64}\ImgDrive\ImgDrive.exe')) or
             FileExists(ExpandConstant('{autopf32}\ImgDrive\ImgDrive.exe'));
end;

function InitializeSetup(): Boolean;
var
   installedVersion: String;
   previous, current: Int64;
begin
   Result := True;
   if RegQueryStringValue(HKCU64, UNINSTALL_KEY, 'DisplayVersion', installedVersion) then begin
      if StrToVersion(installedVersion, previous) and StrToVersion('{#APP_VERSION}', current) then begin
         if ComparePackedVersion(previous, current) > 0 then begin
            SuppressibleMsgBox('A newer FlacCD version is installed. Uninstall it before downgrading.',
                              mbError, MB_OK, IDOK);
            Result := False;
         end;
      end;
   end;
   if Result and not imgDriveDetected() then begin
      SuppressibleMsgBox('ImgDrive was not detected in registered or standard locations. ' +
                        'Install ImgDrive separately from https://www.yubsoft.com/imgdrive/ before mounting. ' +
                        'Custom installations can use FLACCD_IMGDRIVE or --imgdrive. ' +
                        'FlacCD can still be installed; no driver is bundled.', mbInformation, MB_OK, IDOK);
   end;
end;

function PrepareToInstall(var needsRestart: Boolean): String;
var
   executable, registered, manifestHash: String;
begin
   Result := '';
   executable := ExpandConstant('{app}\flaccd.exe');
   { Snapshot the previous manifest before [Files] replaces it. Pre-manifest
     installations retain untracked files rather than guessing ownership. }
   SetArrayLength(oldFiles, 0);
   if FileExists(ExpandConstant('{app}\FlacCDFiles.txt')) then begin
      if RegQueryStringValue(HKCU64, UNINSTALL_KEY, 'FlacCDBinaryManifestSHA256', manifestHash) then begin
         try
            if CompareText(GetSHA256OfFile(ExpandConstant('{app}\FlacCDFiles.txt')), manifestHash) = 0 then begin
               LoadStringsFromFile(ExpandConstant('{app}\FlacCDFiles.txt'), oldFiles);
            end else begin
               Log('Modified binary manifest; obsolete files retained.');
            end;
         except
            Log('Cannot inspect old binary manifest; obsolete files retained.');
         end;
      end;
   end;
   if RegQueryStringValue(HKCU64, SHELL_KEY, 'FlacCD.Executable', registered) then begin
      if CompareText(registered, executable) <> 0 then begin
         Result := 'Explorer integration belongs to another FlacCD location: ' + registered +
                   '. Run shell uninstall there before installing here.';
         Exit;
      end;
   end else if RegKeyExists(HKCU64, SHELL_KEY) then begin
      Result := 'Explorer registration is unowned or unreadable; preserved. Repair it before installing.';
      Exit;
   end;
   if FileExists(executable) then begin
      Result := runFlacCD(executable, 'agent stop --timeout 30');
      if Result = '' then begin
         { Ownership validation happens in the existing CLI, before files change. }
         Result := runFlacCD(executable, 'shell status');
      end;
   end;
end;

procedure CurStepChanged(curStep: TSetupStep);
var
   problem: String;
begin
   if curStep = ssPostInstall then begin
      removeObsoleteBinaries();
      if not RegWriteStringValue(HKCU64, UNINSTALL_KEY, 'FlacCDBinaryManifestSHA256',
                                 GetSHA256OfFile(ExpandConstant('{app}\FlacCDFiles.txt'))) then begin
         Log('Cannot record manifest ownership; future upgrades will retain untracked binaries.');
      end;
      problem := runFlacCD(ExpandConstant('{app}\flaccd.exe'), 'shell install');
      if problem <> '' then begin
         registrationFailed := True;
         Log(problem);
         SuppressibleMsgBox(problem + ' Installation files are present; repair registration with flaccd shell install.',
                           mbError, MB_OK, IDOK);
      end;
   end;
end;

function GetCustomSetupExitCode(): Integer;
begin
   Result := 0;
   if registrationFailed then begin
      Result := 1;
   end;
end;

procedure CurUninstallStepChanged(curStep: TUninstallStep);
var
   executable, registered, problem: String;
begin
   if curStep <> usUninstall then begin
      Exit;
   end;
   executable := ExpandConstant('{app}\flaccd.exe');
   problem := runFlacCD(executable, 'agent stop --timeout 30');
   if problem = '' then begin
      { Do not unregister a different installation's Explorer command. }
      if RegQueryStringValue(HKCU64, SHELL_KEY, 'FlacCD.Executable', registered) then begin
         if CompareText(registered, executable) = 0 then begin
            problem := runFlacCD(executable, 'shell uninstall');
         end;
      end else if RegKeyExists(HKCU64, SHELL_KEY) then begin
         problem := 'Explorer registration is unowned or unreadable; preserved. Repair it before uninstalling.';
      end;
   end;
   if problem <> '' then begin
      SuppressibleMsgBox(problem, mbError, MB_OK, IDOK);
      Abort;
   end else begin
      Log('Agent stopped. Active discs, original FLACs, CUE/backing resources and cache/state are retained.');
   end;
end;
