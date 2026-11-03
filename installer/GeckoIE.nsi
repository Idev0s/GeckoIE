; GeckoIE installer: modern engine (Mypal/Gecko) inside Internet Explorer 6 through a native DocObject view.
Unicode false
SetCompressor /SOLID lzma
RequestExecutionLevel admin
!define APP_NAME "GeckoIE"
!define APP_VERSION "v0.1 Alpha 3"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\GeckoIE"
!define OLD_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\IEBridge"
!define BHO_CLSID "{FC8F2A4B-3C6E-4B53-9D0F-6A1C2E7B5D31}"
!define BHO_KEY "Software\Microsoft\Windows\CurrentVersion\Explorer\Browser Helper Objects\${BHO_CLSID}"
Name "${APP_NAME} ${APP_VERSION}"
OutFile "GeckoIE-v0.1-Alpha3-Setup.exe"
VIProductVersion "0.1.0.2"
VIAddVersionKey "ProductName" "GeckoIE"
VIAddVersionKey "FileDescription" "GeckoIE setup"
VIAddVersionKey "FileVersion" "v0.1 Alpha 3 (build 261101.2)"
VIAddVersionKey "ProductVersion" "v0.1 Alpha 3 (build 261101.2)"
InstallDir "$PROGRAMFILES\GeckoIE"
InstallDirRegKey HKLM "Software\GeckoIE" "InstallDir"
ShowInstDetails show
ShowUninstDetails show
!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "Sections.nsh"
!include "WinVer.nsh"
!include "FileFunc.nsh"
!insertmacro GetSize
!insertmacro DriveSpace
!insertmacro GetRoot
Var MypalDir
Var MypalEdit
Var Token
Var OD1
Var OD2
Var OD3
Var OD4
Var IEMajor
Var OSVista
Var EngMajor
!define MUI_ICON "payload\GeckoIE.ico"
!define MUI_UNICON "payload\GeckoIE.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "GeckoIE v0.1 Alpha 3.$\r$\n$\r$\nThis sets up a modern web engine (Mypal/Gecko) inside Internet Explorer.$\r$\n$\r$\nhttp and https pages open in a native Internet Explorer view: IE's address bar, Back/Forward, history, title and toolbar all stay in sync.$\r$\n$\r$\nOlder GeckoIE / IEBridge installations are detected and replaced. Please close all Internet Explorer windows first."
!define MUI_FINISHPAGE_TEXT "GeckoIE is installed.$\r$\n$\r$\nOpen Internet Explorer and go to any http or https address. Right-click works in pages. An optional toolbar button, GeckoIE Settings, is off by default: View > Toolbars > Customize adds it.$\r$\n$\r$\nFor a plain Internet Explorer window use Start Menu > GeckoIE > Internet Explorer (classic)."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
Page custom MypalPageCreate MypalPageLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; ------------------------------------------------------------------ helpers
!macro MoveOutDir REL
  ${If} ${FileExists} "$INSTDIR\engine\${REL}\*.*"
    RMDir /r "$INSTDIR\engine-removed\${REL}"
    ClearErrors
    Rename "$INSTDIR\engine\${REL}" "$INSTDIR\engine-removed\${REL}"
    DetailPrint "  moved ${REL}\ (folder)"
  ${EndIf}
!macroend

!macro MoveOut REL
  ${If} ${FileExists} "$INSTDIR\engine\${REL}"
    ClearErrors
    Rename "$INSTDIR\engine\${REL}" "$INSTDIR\engine-removed\${REL}"
    DetailPrint "  moved ${REL}"
  ${EndIf}
!macroend

; Unregister whatever an older install left in folder ${DIR} (silent, deletes nothing).
!macro UnregOld DIR
  ${If} ${FileExists} "${DIR}\bho\GeckoIEDoc.dll"
    DetailPrint "  unregistering ${DIR}\bho\GeckoIEDoc.dll"
    nsExec::Exec '"$SYSDIR\regsvr32.exe" /u /s "${DIR}\bho\GeckoIEDoc.dll"'
    Pop $R0
  ${EndIf}
  ${If} ${FileExists} "${DIR}\bho\IEBridgeBho.dll"
    DetailPrint "  unregistering ${DIR}\bho\IEBridgeBho.dll"
    ${If} ${FileExists} "$WINDIR\Microsoft.NET\Framework\v2.0.50727\regasm.exe"
      nsExec::Exec '"$WINDIR\Microsoft.NET\Framework\v2.0.50727\regasm.exe" /unregister "${DIR}\bho\IEBridgeBho.dll"'
    ${Else}
      nsExec::Exec '"$WINDIR\Microsoft.NET\Framework\v4.0.30319\regasm.exe" /unregister "${DIR}\bho\IEBridgeBho.dll"'
    ${EndIf}
    Pop $R0
  ${EndIf}
!macroend

!macro RestoreFile REL
  ${If} ${FileExists} "$INSTDIR\engine-removed\${REL}"
  ${AndIfNot} ${FileExists} "$INSTDIR\engine\${REL}"
    CopyFiles /SILENT "$INSTDIR\engine-removed\${REL}" "$INSTDIR\engine\${REL}"
    DetailPrint "  restored ${REL}"
  ${EndIf}
!macroend
!macro RestoreDir REL
  ${If} ${FileExists} "$INSTDIR\engine-removed\${REL}\*.*"
  ${AndIfNot} ${FileExists} "$INSTDIR\engine\${REL}\*.*"
    CopyFiles /SILENT "$INSTDIR\engine-removed\${REL}\*.*" "$INSTDIR\engine\${REL}"
    DetailPrint "  restored ${REL}\ (folder)"
  ${EndIf}
!macroend
!macro SetIni KEY VAL
  StrCpy $R0 "${KEY}"
  StrCpy $R1 "${VAL}"
  Call SetIniRaw
!macroend

; $R0 = key, $R1 = value written exactly as given (may contain %VARIABLES%)
Function SetIniRaw
  ClearErrors
  FileOpen $2 "$INSTDIR\ieb.ini" r
  FileOpen $3 "$INSTDIR\ieb.ini.tmp" w
  StrLen $5 $R0
  rawLoop:
    FileRead $2 $4
    IfErrors rawDone
    StrCpy $6 $4 $5
    StrCpy $7 $4 1 $5
    ${If} $6 == $R0
    ${AndIf} $7 == "="
      Goto rawLoop
    ${EndIf}
    FileWrite $3 $4
    Goto rawLoop
  rawDone:
  FileWrite $3 "$R0=$R1$\r$\n"
  FileClose $2
  FileClose $3
  Delete "$INSTDIR\ieb.ini"
  Rename "$INSTDIR\ieb.ini.tmp" "$INSTDIR\ieb.ini"
FunctionEnd

Function DetectEnv
  ReadRegStr $0 HKLM "Software\Microsoft\Internet Explorer" "svcVersion"
  ${If} $0 == ""
    ReadRegStr $0 HKLM "Software\Microsoft\Internet Explorer" "Version"
  ${EndIf}
  StrCpy $IEMajor 6
  ${If} $0 != ""
    StrCpy $1 $0 2
    StrCpy $2 $1 1 1
    ${If} $2 == "."
      StrCpy $IEMajor $0 1
    ${Else}
      StrCpy $IEMajor $1
    ${EndIf}
  ${EndIf}
  StrCpy $OSVista 0
  ${If} ${AtLeastWinVista}
    StrCpy $OSVista 1
  ${EndIf}
FunctionEnd

Function AddOld
  ${If} $0 == ""
    Return
  ${EndIf}
  ${IfNot} ${FileExists} "$0\*.*"
    Return
  ${EndIf}
  ${If} $0 == $INSTDIR
    Return
  ${EndIf}
  ${If} $0 == $OD1
  ${OrIf} $0 == $OD2
  ${OrIf} $0 == $OD3
  ${OrIf} $0 == $OD4
    Return
  ${EndIf}
  ${If} $OD1 == ""
    StrCpy $OD1 $0
  ${ElseIf} $OD2 == ""
    StrCpy $OD2 $0
  ${ElseIf} $OD3 == ""
    StrCpy $OD3 $0
  ${ElseIf} $OD4 == ""
    StrCpy $OD4 $0
  ${EndIf}
FunctionEnd

; $0 = old folder: ask before deleting it (never in silent mode, never the folder we install into)
Function AskRemove
  ${If} $0 == ""
    Return
  ${EndIf}
  ${If} $0 == $INSTDIR
    Return
  ${EndIf}
  ${IfNot} ${FileExists} "$0\*.*"
    Return
  ${EndIf}
  IfSilent askDone
  MessageBox MB_YESNO|MB_ICONQUESTION "An older GeckoIE / IEBridge installation was found:$\r$\n$\r$\n$0$\r$\n$\r$\nIts Internet Explorer add-on is already unregistered and the new install has its own copy of the engine.$\r$\n$\r$\nDelete this old folder (engine copy, logs, profile)?" IDNO askDone
  DetailPrint "Removing old folder $0"
  RMDir /r "$0"
  askDone:
FunctionEnd


Function MypalPageCreate
  ${If} "$MypalDir" == "$INSTDIR\engine"
    Abort
  ${EndIf}
  !insertmacro MUI_HEADER_TEXT "Mypal engine" "Choose the folder that contains your Mypal files."
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}
  ${NSD_CreateLabel} 0 0 100% 36u "GeckoIE copies the Mypal engine into its own folder. Select the folder that contains firefox.exe or mypal.exe and xul.dll (for example C:\Program Files\Firefox)."
  Pop $0
  ${NSD_CreateDirRequest} 0 46u 78% 12u "$MypalDir"
  Pop $MypalEdit
  ${NSD_CreateBrowseButton} 80% 45u 20% 14u "Browse..."
  Pop $0
  ${NSD_OnClick} $0 MypalBrowse
  nsDialogs::Show
FunctionEnd
Function MypalBrowse
  ${NSD_GetText} $MypalEdit $0
  nsDialogs::SelectFolderDialog "Select the Mypal folder" "$0"
  Pop $0
  ${If} $0 != error
    ${NSD_SetText} $MypalEdit $0
  ${EndIf}
FunctionEnd
Function MypalPageLeave
  ${NSD_GetText} $MypalEdit $MypalDir
  ${IfNot} ${FileExists} "$MypalDir\firefox.exe"
  ${AndIfNot} ${FileExists} "$MypalDir\mypal.exe"
    MessageBox MB_OK|MB_ICONEXCLAMATION "Neither firefox.exe nor mypal.exe was found in that folder."
    Abort
  ${EndIf}
  ${IfNot} ${FileExists} "$MypalDir\xul.dll"
    MessageBox MB_OK|MB_ICONEXCLAMATION "xul.dll was not found in that folder."
    Abort
  ${EndIf}
  ${If} "$MypalDir" != "$INSTDIR\engine"
    ${GetSize} "$MypalDir" "/S=0K" $0 $1 $2
    ${GetRoot} "$INSTDIR" $3
    ${DriveSpace} "$3\" "/D=F /S=K" $4
    IntOp $5 $0 / 4
    IntOp $5 $5 + $0
    IntOp $5 $5 + 40960
    ${If} $4 < $5
      IntOp $6 $5 / 1024
      IntOp $7 $4 / 1024
      MessageBox MB_YESNO|MB_ICONEXCLAMATION "There may not be enough free disk space on the drive you chose: about $6 MB are needed, $7 MB are free.$\r$\n$\r$\nContinue anyway?" IDYES diskOk
      Abort
      diskOk:
    ${EndIf}
  ${EndIf}
FunctionEnd

; ------------------------------------------------------------------ install
Section "Engine, Internet Explorer add-on and native view (required)" SecMain
  SectionIn RO
  ieCheck:
  StrCpy $R9 0
  FindWindow $0 "IEFrame"
  ${If} $0 != 0
    StrCpy $R9 1
  ${EndIf}
  ${If} $R9 == 0   ; an Internet Explorer process without a window (background, or still shutting down) keeps the add-on files in use too
    nsExec::Exec '"$SYSDIR\cmd.exe" /c ""$SYSDIR\tasklist.exe" /FI "IMAGENAME eq iexplore.exe" /NH | "$SYSDIR\find.exe" /I "iexplore.exe" >nul"'
    Pop $0
    ${If} $0 == 0
      StrCpy $R9 1
    ${EndIf}
  ${EndIf}
  ${If} $R9 == 1
    MessageBox MB_ABORTRETRYIGNORE|MB_ICONEXCLAMATION "Internet Explorer is still running (possibly in the background).$\r$\n$\r$\nClose all Internet Explorer windows, then click Retry.$\r$\nIgnore closes Internet Explorer for you (unsaved work in it is lost).$\r$\nAbort cancels the installation." /SD IDABORT IDRETRY ieCheck IDIGNORE ieKill
    Abort "Installation cancelled."
    ieKill:
    nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM iexplore.exe'
    Pop $0
    nsExec::Exec '"$SYSDIR\tskill.exe" iexplore'
    Pop $0
    Sleep 1500
    Goto ieCheck
  ${EndIf}

  DetailPrint "Looking for older GeckoIE / IEBridge installations..."
  nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM ieengine.exe'
  Pop $0
  nsExec::Exec '"$SYSDIR\tskill.exe" ieengine'
  Pop $0
  !insertmacro UnregOld "$INSTDIR"
  !insertmacro UnregOld "$OD1"
  !insertmacro UnregOld "$OD2"
  !insertmacro UnregOld "$OD3"
  !insertmacro UnregOld "$OD4"
  DeleteRegKey HKLM "${BHO_KEY}"
  DeleteRegKey HKCR "CLSID\${BHO_CLSID}"
  DeleteRegKey HKCR "IEBridge.Bho"
  DeleteRegKey HKLM "${OLD_UNINST_KEY}"
  DeleteRegKey HKLM "Software\IEBridge"
  RMDir /r "$SMPROGRAMS\IEBridge"
  ${If} $OD1 != ""
    DetailPrint "  older installation(s) found; their folders are offered for removal at the end"
  ${EndIf}

  SetOutPath "$INSTDIR"
  CreateDirectory "$INSTDIR\engine"
  ${If} "$MypalDir" != "$INSTDIR\engine"
    Call CopyEngine
  ${EndIf}
  ${IfNot} ${FileExists} "$INSTDIR\engine\xul.dll"
    MessageBox MB_OK|MB_ICONSTOP "The engine files could not be copied."
    Abort "Copying the engine failed."
  ${EndIf}
  ${If} ${FileExists} "$INSTDIR\engine\firefox.exe"
    CopyFiles /SILENT "$INSTDIR\engine\firefox.exe" "$INSTDIR\engine\ieengine.exe"
  ${ElseIf} ${FileExists} "$INSTDIR\engine\mypal.exe"
    CopyFiles /SILENT "$INSTDIR\engine\mypal.exe" "$INSTDIR\engine\ieengine.exe"
  ${EndIf}
  ${IfNot} ${FileExists} "$INSTDIR\engine\ieengine.exe"
    MessageBox MB_OK|MB_ICONSTOP "ieengine.exe could not be created."
    Abort "Creating ieengine.exe failed."
  ${EndIf}

  DetailPrint "Installing the bridge files..."
  SetOutPath "$INSTDIR\engine-overlay"
  File "payload\overlay\iebridge.cfg"
  SetOutPath "$INSTDIR\engine-overlay\defaults\pref"
  File "payload\overlay\defaults\pref\autoconfig.js"
  SetOutPath "$INSTDIR\engine-overlay\iebridge"
  File "payload\overlay\iebridge\chrome.manifest"
  SetOutPath "$INSTDIR\engine-overlay\iebridge\content"
  File "payload\overlay\iebridge\content\bridge.jsm"
  CopyFiles /SILENT "$INSTDIR\engine-overlay\*.*" "$INSTDIR\engine"
  SetOutPath "$INSTDIR\bare-app"
  File /r "payload\bare-app\*.*"
  SetOutPath "$INSTDIR\bare-app-extra"
  File "payload\bare-app-extra\prefs-xul.js"
  File "payload\bare-app-extra\perf.js"
  ReadINIStr $0 "$INSTDIR\engine\application.ini" "App" "Version"
  StrCpy $EngMajor 74
  ${If} $0 != ""
    StrCpy $1 $0 2
    StrCpy $2 $1 1 1
    ${If} $2 == "."
      StrCpy $EngMajor $0 1
    ${Else}
      StrCpy $EngMajor $1
    ${EndIf}
  ${EndIf}
  DetailPrint "Engine version $0 (major $EngMajor)"
  ${If} $EngMajor < 69
    DetailPrint "  older engine: using the XUL window variant"
    CopyFiles /SILENT "$INSTDIR\bare-app-extra\prefs-xul.js" "$INSTDIR\bare-app\defaults\preferences\prefs.js"
  ${EndIf}
  RMDir /r "$INSTDIR\bho-src"   ; the add-on is native code now: no source to build on this computer
  SetOutPath "$INSTDIR"
  File "payload\restore-engine.bat"
  File "payload\shell.html"
  File "payload\classic-ie.cmd"
  File "payload\set-ini.bat"
  File "payload\omnislim.exe"
  File "payload\purge-cache.bat"
  Delete "$INSTDIR\engine\iebridge\content\bridge.js"
  Delete "$INSTDIR\engine-overlay\iebridge\content\bridge.js"

  System::Call 'kernel32::GetTickCount()i.r1'
  System::Call 'kernel32::GetCurrentProcessId()i.r2'
  IntFmt $3 "%08x" $1
  IntFmt $4 "%08x" $2
  StrCpy $Token "$3$4"
  ${If} ${FileExists} "$INSTDIR\ieb.ini"
    DetailPrint "Keeping your settings (ieb.ini), switching it to the native view..."
    !insertmacro SetIni "EngineDir" "engine"
    !insertmacro SetIni "EngineExe" "ieengine.exe"
    !insertmacro SetIni "EngineMode" "inproc"
    !insertmacro SetIni "HostMode" "docobj"
    !insertmacro SetIni "Enabled" "1"
    Goto settingsDone
  ${EndIf}
  DetailPrint "Writing settings..."
  FileOpen $0 "$INSTDIR\ieb.ini" w
  FileWrite $0 "; GeckoIE settings$\r$\n"
  FileWrite $0 "EngineDir=engine$\r$\n"
  FileWrite $0 "EngineExe=ieengine.exe$\r$\n"
  FileWrite $0 "Port=38650$\r$\n"
  FileWrite $0 "Token=$Token$\r$\n"
  FileWrite $0 "Home=about:blank$\r$\n"
  FileWrite $0 "Kiosk=1$\r$\n"
  FileWrite $0 "; 1 = start the engine when Internet Explorer opens$\r$\n"
  FileWrite $0 "Prewarm=1$\r$\n"
  FileWrite $0 "; 0 = turn GeckoIE off and use Internet Explorer's own rendering$\r$\n"
  FileWrite $0 "Enabled=1$\r$\n"
  FileWrite $0 "; inproc = engine runs inside iexplore.exe; docobj = http(s) opens in the native IE view (gie:)$\r$\n"
  FileWrite $0 "EngineMode=inproc$\r$\n"
  FileWrite $0 "HostMode=docobj$\r$\n"
  FileWrite $0 "ProfileDir=%APPDATA%\GeckoIE\profile$\r$\n"
  FileWrite $0 "Priority=belownormal$\r$\n"
  FileWrite $0 "MaxWorkingSetMB=0$\r$\n"
  FileWrite $0 "TitleSuffix=Microsoft Internet Explorer$\r$\n"
  FileClose $0
  settingsDone:

  WriteRegStr HKLM "Software\GeckoIE" "InstallDir" "$INSTDIR"
  ${If} "$MypalDir" != "$INSTDIR\engine"
    WriteRegStr HKLM "Software\GeckoIE" "MypalSource" "$MypalDir"
  ${EndIf}
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateDirectory "$SMPROGRAMS\GeckoIE"
  CreateShortCut "$SMPROGRAMS\GeckoIE\Internet Explorer (classic).lnk" "$INSTDIR\classic-ie.cmd" "" "$PROGRAMFILES\Internet Explorer\iexplore.exe" 0 SW_SHOWMINIMIZED
  CreateShortCut "$SMPROGRAMS\GeckoIE\Uninstall GeckoIE.lnk" "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "GeckoIE (modern engine for Internet Explorer)"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\bho\GeckoIE.ico"
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1

  SetOutPath "$INSTDIR\bho"
  Delete "$INSTDIR\bho\IEBridgeBho.dll"   ; the older .NET add-on
  File "payload\GeckoHost.dll"
  File "payload\GeckoIEDoc.dll"
  File "payload\GeckoIE.ico"
  DetailPrint "Registering the native Internet Explorer view (gie:)..."
  nsExec::ExecToLog '"$SYSDIR\regsvr32.exe" /s "$INSTDIR\bho\GeckoIEDoc.dll"'
  Pop $0
  ${If} $0 != 0
    MessageBox MB_OK|MB_ICONEXCLAMATION "The native view could not be registered (regsvr32 error $0). Internet Explorer will keep using its own rendering until GeckoIEDoc.dll is registered (run: regsvr32 $\"$INSTDIR\bho\GeckoIEDoc.dll$\")."
  ${EndIf}

  ; older installations: ask before deleting their folders
  StrCpy $0 $OD1
  Call AskRemove
  StrCpy $0 $OD2
  Call AskRemove
  StrCpy $0 $OD3
  Call AskRemove
  StrCpy $0 $OD4
  Call AskRemove
SectionEnd

SectionGroup "Optional components"
  Section "PDF viewer (pdf.js)" SecPdf
    DetailPrint "PDF viewer: kept"
  SectionEnd
  Section /o "Developer tools (adds Inspect Element to the right-click menu)" SecDev
    DetailPrint "Developer tools: kept"
  SectionEnd
  Section "Media playback (H.264 / AAC)" SecMedia
    DetailPrint "Media playback: kept"
  SectionEnd
  Section "Spell-check dictionaries" SecDict
    DetailPrint "Dictionaries: kept"
  SectionEnd
  Section "Performance tuning for old computers (memory cache, fewer background features)" SecPerf
    DetailPrint "Performance tuning: selected"
  SectionEnd
  Section "Toolbar button 'GeckoIE Settings' (hidden until you add it)" SecBtn
    DetailPrint "Toolbar button: registered"
  SectionEnd
SectionGroupEnd

SectionGroup "Slimming"
  Section /o "Keep a backup of removed engine files (restore-engine.bat; uses extra disk space)" SecKeep
    DetailPrint "Backups of removed files will be kept"
  SectionEnd
  Section /o "Remove files GeckoIE never uses (updater, crash reporter, Firefox browser UI folder)" SecSlim
    DetailPrint "Basic slimming selected"
  SectionEnd
  Section /o "Extended slimming: also remove Sync, Firefox Account, telemetry start-up, add-on manager and update screens (faster start)" SecSlimX
    DetailPrint "Extended slimming selected"
  SectionEnd
SectionGroupEnd


Section "-Finalize" SecFinal
  CreateDirectory "$INSTDIR\engine-removed"
  ; ---- optional components that are plain files
  SectionGetFlags ${SecMedia} $1
  IntOp $1 $1 & ${SF_SELECTED}
  ${If} $1 == 0
    !insertmacro MoveOut "mozavcodec.dll"
    !insertmacro MoveOut "mozavutil.dll"
  ${Else}
    !insertmacro RestoreFile "mozavcodec.dll"
    !insertmacro RestoreFile "mozavutil.dll"
  ${EndIf}
  SectionGetFlags ${SecSlimX} $1
  IntOp $1 $1 & ${SF_SELECTED}
  ${If} $1 != 0
    !insertmacro MoveOutDir "hyphenation"
  ${Else}
    !insertmacro RestoreDir "hyphenation"
  ${EndIf}
  SectionGetFlags ${SecDict} $1
  IntOp $1 $1 & ${SF_SELECTED}
  ${If} $1 == 0
    !insertmacro MoveOutDir "dictionaries"
  ${Else}
    !insertmacro RestoreDir "dictionaries"
  ${EndIf}
  ; ---- engine core (omni.ja): start from the original and remove what was not chosen
  FileOpen $0 "$INSTDIR\omni-remove.txt" w
  FileWrite $0 "# generated by the installer from the components you chose$\r$\n"
  StrCpy $R2 0
  SectionGetFlags ${SecDev} $1
  IntOp $1 $1 & ${SF_SELECTED}
  ${If} $1 == 0
    StrCpy $R2 1
    FileWrite $0 "chrome/devtools/$\r$\nmodules/devtools/$\r$\nchrome/remote/$\r$\nchrome/en-US/locale/en-US/devtools/$\r$\nchrome/en-US/locale/en-US/remote/$\r$\n"
  ${EndIf}
  SectionGetFlags ${SecPdf} $1
  IntOp $1 $1 & ${SF_SELECTED}
  ${If} $1 == 0
    StrCpy $R2 1
    FileWrite $0 "chrome/pdfjs/$\r$\nchrome/en-US/locale/en-US/pdfviewer/$\r$\n"
  ${EndIf}
  SectionGetFlags ${SecSlimX} $1
  IntOp $1 $1 & ${SF_SELECTED}
  ${If} $1 != 0   ; extended slimming: parts of the Firefox browser that a page view never starts, so the engine has less to read and register at start-up
    StrCpy $R2 1
    FileWrite $0 "components/TelemetryStartup$\r$\ncomponents/Weave$\r$\ncomponents/SyncComponents$\r$\ncomponents/FxAccounts$\r$\nmodules/services-sync/$\r$\nmodules/FxAccounts$\r$\nchrome/toolkit/content/mozapps/extensions/$\r$\nchrome/toolkit/content/mozapps/update/$\r$\n"
  ${EndIf}
  FileClose $0
  StrCpy $R1 "$INSTDIR\engine\omni.ja"
  ${If} ${FileExists} "$INSTDIR\engine-removed\omni.ja"
    StrCpy $R1 "$INSTDIR\engine-removed\omni.ja"
  ${EndIf}
  ${If} $R2 == 1
    ${If} ${FileExists} "$R1"
      DetailPrint "Repacking omni.ja without the components you did not choose..."
      nsExec::ExecToLog '"$INSTDIR\omnislim.exe" "$R1" "$INSTDIR\engine\omni.ja.slim" "$INSTDIR\omni-remove.txt"'
      Pop $0
      ${If} $0 == 0
        ${IfNot} ${FileExists} "$INSTDIR\engine-removed\omni.ja"
          Rename "$INSTDIR\engine\omni.ja" "$INSTDIR\engine-removed\omni.ja"
        ${Else}
          Delete "$INSTDIR\engine\omni.ja"
        ${EndIf}
        Rename "$INSTDIR\engine\omni.ja.slim" "$INSTDIR\engine\omni.ja"
        DetailPrint "  done (restore-engine.bat puts the original back)"
      ${ElseIf} $0 == 2
        DetailPrint "  nothing to remove from this engine"
      ${Else}
        Delete "$INSTDIR\engine\omni.ja.slim"
        DetailPrint "  repacking failed (code $0), the engine was left as it was"
      ${EndIf}
    ${Else}
      DetailPrint "engine\omni.ja not found (already unpacked?) - not repacked"
    ${EndIf}
  ${Else}
    ${If} ${FileExists} "$INSTDIR\engine-removed\omni.ja"
      CopyFiles /SILENT "$INSTDIR\engine-removed\omni.ja" "$INSTDIR\engine\omni.ja"
      DetailPrint "Original omni.ja restored (everything was chosen)"
    ${EndIf}
  ${EndIf}
  ; ---- developer tools live in browser\omni.ja (Firefox's own app part), not in the engine core
  SectionGetFlags ${SecDev} $1
  IntOp $1 $1 & ${SF_SELECTED}
  ${If} $1 != 0
    !insertmacro RestoreDir "browser"
  ${EndIf}
  StrCpy $R3 "$INSTDIR\engine\browser\omni.ja"
  ${If} ${FileExists} "$R3"
    ${IfNot} ${FileExists} "$INSTDIR\engine-removed\browser-omni.ja.orig"
      CopyFiles /SILENT "$R3" "$INSTDIR\engine-removed\browser-omni.ja.orig"
    ${EndIf}
    ${If} $1 == 0
      DetailPrint "Removing the developer tools from browser\omni.ja..."
      FileOpen $0 "$INSTDIR\omni-remove-browser.txt" w
      FileWrite $0 "chrome/devtools/$\r$\nmodules/devtools/$\r$\n"
      FileClose $0
      nsExec::ExecToLog '"$INSTDIR\omnislim.exe" "$INSTDIR\engine-removed\browser-omni.ja.orig" "$INSTDIR\engine\browser\omni.ja.slim" "$INSTDIR\omni-remove-browser.txt"'
      Pop $0
      ${If} $0 == 0
        Delete "$R3"
        Rename "$INSTDIR\engine\browser\omni.ja.slim" "$R3"
      ${Else}
        Delete "$INSTDIR\engine\browser\omni.ja.slim"
      ${EndIf}
    ${Else}
      CopyFiles /SILENT "$INSTDIR\engine-removed\browser-omni.ja.orig" "$R3"
    ${EndIf}
  ${EndIf}
  ; does this engine contain developer tools at all? (look at the untouched copies; only needed when they were asked for)
  StrCpy $R4 0
  SectionGetFlags ${SecDev} $1
  IntOp $1 $1 & ${SF_SELECTED}
  ${If} $1 != 0
  StrCpy $R5 "$INSTDIR\engine-removed\browser-omni.ja.orig"
  ${IfNot} ${FileExists} "$R5"
    StrCpy $R5 "$INSTDIR\engine\browser\omni.ja"
  ${EndIf}
  ${IfNot} ${FileExists} "$R5"
    StrCpy $R5 "$INSTDIR\engine-removed\browser\omni.ja"
  ${EndIf}
  ${If} ${FileExists} "$R5"
    nsExec::Exec '"$INSTDIR\omnislim.exe" --has "$R5" chrome/devtools/'
    Pop $0
    ${If} $0 == 0
      StrCpy $R4 1
    ${EndIf}
    nsExec::Exec '"$INSTDIR\omnislim.exe" --has "$R5" modules/devtools/'
    Pop $0
    ${If} $0 == 0
      StrCpy $R4 1
    ${EndIf}
  ${EndIf}
  ${If} ${FileExists} "$R1"
    nsExec::Exec '"$INSTDIR\omnislim.exe" --has "$R1" chrome/devtools/'
    Pop $0
    ${If} $0 == 0
      StrCpy $R4 1
    ${EndIf}
  ${EndIf}
  ${EndIf}
  ; ---- basic slimming
  SectionGetFlags ${SecSlim} $1
  IntOp $1 $1 & ${SF_SELECTED}
  ${If} $1 != 0
    DetailPrint "Moving unneeded engine files to $INSTDIR\engine-removed ..."
    !insertmacro MoveOut "crashreporter.exe"
    !insertmacro MoveOut "crashreporter.ini"
    !insertmacro MoveOut "crashreporter-override.ini"
    !insertmacro MoveOut "maintenanceservice.exe"
    !insertmacro MoveOut "maintenanceservice_installer.exe"
    !insertmacro MoveOut "updater.exe"
    !insertmacro MoveOut "updater.ini"
    !insertmacro MoveOut "update-settings.ini"
    !insertmacro MoveOut "pingsender.exe"
    !insertmacro MoveOut "minidump-analyzer.exe"
    !insertmacro MoveOut "precomplete"
    !insertmacro MoveOut "removed-files"
    SectionGetFlags ${SecDev} $2
    IntOp $2 $2 & ${SF_SELECTED}
    ${If} $2 == 0
      !insertmacro MoveOutDir "browser"
    ${Else}
      DetailPrint "  keeping the browser folder: the developer tools live there"
    ${EndIf}
    !insertmacro MoveOutDir "gmp-clearkey"
    !insertmacro MoveOutDir "uninstall"
  ${EndIf}
  ; ---- toolbar button
  SectionGetFlags ${SecBtn} $1
  IntOp $1 $1 & ${SF_SELECTED}
  ${If} $1 == 0
    DeleteRegKey HKLM "Software\Microsoft\Internet Explorer\Extensions\{C3A1F0D2-6B4E-4C7A-9E58-2D1B0F7A6E33}"
    DetailPrint "Toolbar button not registered"
  ${EndIf}
  ; ---- settings the engine and the add-on read
  SectionGetFlags ${SecDev} $1
  IntOp $1 $1 & ${SF_SELECTED}
  ${If} $1 != 0
  ${AndIf} $R4 == 0
    DetailPrint "This engine does not contain the Firefox developer tools, so Inspect Element is not offered."
    IfSilent devNone
    MessageBox MB_OK|MB_ICONINFORMATION "The engine you installed does not contain the Firefox developer tools, so Inspect Element will not be offered."
    devNone:
    StrCpy $1 0
  ${EndIf}
  ${If} $1 == 0
    !insertmacro SetIni "DevTools" "0"
  ${Else}
    !insertmacro SetIni "DevTools" "1"
  ${EndIf}
  ${If} $IEMajor >= 11
    StrCpy $R1 "Internet Explorer"
  ${ElseIf} $IEMajor >= 7
    StrCpy $R1 "Windows Internet Explorer"
  ${Else}
    StrCpy $R1 "Microsoft Internet Explorer"
  ${EndIf}
  StrCpy $R0 "TitleSuffix"
  Call SetIniRaw
  ${If} $OSVista == 1
    StrCpy $R0 "ProfileDir"
    StrCpy $R1 "%USERPROFILE%\AppData\LocalLow\GeckoIE\profile"
    Call SetIniRaw
    DetailPrint "Windows Vista or later: the engine profile goes to LocalLow (Protected Mode)"
  ${EndIf}
  ; ---- performance preferences and the environment (everything adapts by itself at run time)
  SectionGetFlags ${SecPerf} $1
  IntOp $1 $1 & ${SF_SELECTED}
  ${If} $1 == 0
    Delete "$INSTDIR\bare-app\defaults\preferences\perf.js"
  ${Else}
    CopyFiles /SILENT "$INSTDIR\bare-app-extra\perf.js" "$INSTDIR\bare-app\defaults\preferences\perf.js"
  ${EndIf}
  Delete "$INSTDIR\gh-start.flag"
  DetailPrint "Windows Vista or later: $OSVista; Internet Explorer $IEMajor; engine version $EngMajor"
  ${If} $OSVista == 1
  ${OrIf} $IEMajor >= 7
    DetailPrint "  not Windows XP / IE 6: GeckoIE adapts at run time (experimental there)"
  ${EndIf}
  ${If} $EngMajor != 74
    DetailPrint "  not Mypal 74: the engine is started in experimental mode (see gh-log.txt if it does not start)"
  ${EndIf}
  ; ---- backups only when asked for (run setup again with your Mypal folder to bring anything back)
  SectionGetFlags ${SecKeep} $1
  IntOp $1 $1 & ${SF_SELECTED}
  ${If} $1 == 0
    RMDir /r "$INSTDIR\engine-removed"
  ${EndIf}
SectionEnd

Function CopyEngine   ; after the components, because it reads which of them were chosen
    DetailPrint "Copying the Mypal engine (files that did not change are skipped)..."
    RMDir /r "$INSTDIR\engine-removed"
    ; files that the chosen components move away again anyway are not copied (unless a backup was asked for)
    FileOpen $0 "$INSTDIR\xcopy-exclude.txt" w
    FileWrite $0 "\omni.ja.slim$\r$\n"
    SectionGetFlags ${SecKeep} $1
    IntOp $1 $1 & ${SF_SELECTED}
    ${If} $1 == 0
      SectionGetFlags ${SecMedia} $2
      IntOp $2 $2 & ${SF_SELECTED}
      ${If} $2 == 0
        FileWrite $0 "\mozavcodec.dll$\r$\n\mozavutil.dll$\r$\n"
      ${EndIf}
      SectionGetFlags ${SecDict} $2
      IntOp $2 $2 & ${SF_SELECTED}
      ${If} $2 == 0
        FileWrite $0 "\dictionaries\$\r$\n"
      ${EndIf}
      SectionGetFlags ${SecSlimX} $2
      IntOp $2 $2 & ${SF_SELECTED}
      ${If} $2 != 0
        FileWrite $0 "\hyphenation\$\r$\n"
      ${EndIf}
      SectionGetFlags ${SecSlim} $2
      IntOp $2 $2 & ${SF_SELECTED}
      ${If} $2 != 0
        FileWrite $0 "\crashreporter.exe$\r$\n\crashreporter.ini$\r$\n\crashreporter-override.ini$\r$\n\maintenanceservice.exe$\r$\n\maintenanceservice_installer.exe$\r$\n\updater.exe$\r$\n\updater.ini$\r$\n\update-settings.ini$\r$\n\pingsender.exe$\r$\n\minidump-analyzer.exe$\r$\n\precomplete$\r$\n\removed-files$\r$\n\gmp-clearkey\$\r$\n\uninstall\$\r$\n"
        SectionGetFlags ${SecDev} $2
        IntOp $2 $2 & ${SF_SELECTED}
        ${If} $2 == 0
          FileWrite $0 "\browser\$\r$\n"
        ${EndIf}
      ${EndIf}
    ${EndIf}
    FileClose $0
    SetOutPath "$INSTDIR"
    nsExec::ExecToLog '"$SYSDIR\xcopy.exe" "$MypalDir\*.*" "$INSTDIR\engine" /E /I /Y /Q /D /R /EXCLUDE:xcopy-exclude.txt'   ; relative path: the current folder is $INSTDIR, and xcopy cannot take a path with spaces here
    Pop $0
    ${If} $0 != 0
    ${AndIf} $0 != 1
      DetailPrint "  quick copy did not work (code $0): copying everything"
      CopyFiles /SILENT "$MypalDir\*.*" "$INSTDIR\engine"
    ${EndIf}
    ; the files that repacking changes are always taken fresh from the source
    ${If} ${FileExists} "$MypalDir\omni.ja"
      CopyFiles /SILENT "$MypalDir\omni.ja" "$INSTDIR\engine\omni.ja"
    ${EndIf}
    ${If} ${FileExists} "$MypalDir\browser\omni.ja"
    ${AndIf} ${FileExists} "$INSTDIR\engine\browser\*.*"
      CopyFiles /SILENT "$MypalDir\browser\omni.ja" "$INSTDIR\engine\browser\omni.ja"
    ${EndIf}
    Delete "$INSTDIR\xcopy-exclude.txt"
FunctionEnd

!ifndef LVM_GETITEMCOUNT
  !define LVM_GETITEMCOUNT 0x1004
!endif
!ifndef LVM_GETITEMTEXT
  !define LVM_GETITEMTEXT 0x102D
!endif
Function DumpLog
  Exch $5
  Push $0
  Push $1
  Push $2
  Push $3
  Push $4
  Push $6
  FindWindow $0 "#32770" "" $HWNDPARENT
  GetDlgItem $0 $0 1016
  StrCmp $0 0 dumpExit
  FileOpen $5 $5 "w"
  StrCmp $5 "" dumpExit
    SendMessage $0 ${LVM_GETITEMCOUNT} 0 0 $6
    System::Alloc ${NSIS_MAX_STRLEN}
    Pop $3
    StrCpy $2 0
    System::Call "*(i, i, i, i, i, i, i, i, i) i (0, 0, 0, 0, 0, r3, ${NSIS_MAX_STRLEN}) .r1"
    dumpLoop: StrCmp $2 $6 dumpDone
      System::Call "User32::SendMessageA(i, i, i, i) i ($0, ${LVM_GETITEMTEXT}, $2, r1)"
      System::Call "*$3(&t${NSIS_MAX_STRLEN} .r4)"
      FileWrite $5 "$4$\r$\n"
      IntOp $2 $2 + 1
      Goto dumpLoop
    dumpDone:
      FileClose $5
      System::Free $1
      System::Free $3
  dumpExit:
  Pop $6
  Pop $4
  Pop $3
  Pop $2
  Pop $1
  Pop $0
  Pop $5
FunctionEnd

Function .onInstSuccess
  Push "$INSTDIR\setup-log.txt"
  Call DumpLog
FunctionEnd

Function .onInit
  StrCpy $MypalDir ""
  StrCpy $OD1 ""
  StrCpy $OD2 ""
  StrCpy $OD3 ""
  StrCpy $OD4 ""
  ReadRegStr $0 HKLM "Software\GeckoIE" "InstallDir"
  Call AddOld
  ReadRegStr $0 HKLM "Software\IEBridge" "InstallDir"
  Call AddOld
  ReadRegStr $0 HKLM "${OLD_UNINST_KEY}" "InstallLocation"
  Call AddOld
  ReadRegStr $0 HKLM "${UNINST_KEY}" "InstallLocation"
  Call AddOld
  StrCpy $0 "$PROGRAMFILES\IEBridge"
  Call AddOld
  StrCpy $0 "$PROGRAMFILES\GeckoIE"
  Call AddOld
  StrCpy $0 "C:\iebridge"
  Call AddOld
  ; where to take the engine from: the folder used last time (a fresh copy each time keeps components switchable), else the installed one
  ReadRegStr $0 HKLM "Software\GeckoIE" "MypalSource"
  ${If} $0 != ""
  ${AndIf} ${FileExists} "$0\xul.dll"
    StrCpy $MypalDir $0
  ${ElseIf} ${FileExists} "$INSTDIR\engine\xul.dll"
    StrCpy $MypalDir "$INSTDIR\engine"
  ${ElseIf} ${FileExists} "$OD1\engine\xul.dll"
    StrCpy $MypalDir "$OD1\engine"
  ${ElseIf} ${FileExists} "$OD2\engine\xul.dll"
    StrCpy $MypalDir "$OD2\engine"
  ${ElseIf} ${FileExists} "$OD3\engine\xul.dll"
    StrCpy $MypalDir "$OD3\engine"
  ${ElseIf} ${FileExists} "$OD4\engine\xul.dll"
    StrCpy $MypalDir "$OD4\engine"
  ${ElseIf} ${FileExists} "C:\iebridge\engine-mypal\xul.dll"
    StrCpy $MypalDir "C:\iebridge\engine-mypal"
  ${ElseIf} ${FileExists} "$PROGRAMFILES\Mypal\xul.dll"
    StrCpy $MypalDir "$PROGRAMFILES\Mypal"
  ${ElseIf} ${FileExists} "$PROGRAMFILES\Firefox\xul.dll"
    StrCpy $MypalDir "$PROGRAMFILES\Firefox"
  ${EndIf}
  Call DetectEnv
FunctionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "The engine, the Internet Explorer add-on and the native view. Required."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPdf} "Built-in PDF viewer. Without it PDF files are not shown inside pages (saves about 3 MB)."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDev} "Gecko developer tools and the Inspect Element item in the right-click menu (experimental). They are kept in the engine's browser folder (which the slimming option then leaves alone). Only works if your engine contains them. Without them the tools are removed from the engine."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMedia} "H.264 and AAC playback libraries. Without them many MP4 videos do not play."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDict} "English spell-check dictionaries for text fields."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPerf} "Memory cache instead of the slow disk cache, no service workers / WebGL / telemetry / update checks, no slow-script prompts. Recommended for old computers."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecBtn} "Registers the GeckoIE Settings toolbar button. It stays hidden until you add it (View > Toolbars > Customize)."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecKeep} "Keeps everything the installer removes in the engine-removed folder, so restore-engine.bat can put it back. Off by default: it needs extra disk space. To get removed parts back later, run setup again and choose your original Mypal folder."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecSlim} "Moves the updater, crash reporter and Firefox's own browser interface folder to a backup folder. Not needed by GeckoIE."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecSlimX} "Goes further than basic slimming and trades features for speed: the engine no longer contains Firefox Sync, Firefox Accounts, the telemetry start-up service, the add-on manager and update screens, and the automatic hyphenation tables. GeckoIE does not use any of them, so the engine has less to read and register when it starts. Everything is kept in a backup folder; restore-engine.bat puts the original back."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---------------------------------------------------------------- uninstall
Function un.onInit
  unCheckIE:
  StrCpy $R9 0
  FindWindow $0 "IEFrame"
  ${If} $0 != 0
    StrCpy $R9 1
  ${Else}   ; an Internet Explorer process without a window keeps the add-on files in use as well
    nsExec::Exec '"$SYSDIR\cmd.exe" /c ""$SYSDIR\tasklist.exe" /FI "IMAGENAME eq iexplore.exe" /NH | "$SYSDIR\find.exe" /I "iexplore.exe" >nul"'
    Pop $0
    ${If} $0 == 0
      StrCpy $R9 1
    ${EndIf}
  ${EndIf}
  ${If} $R9 == 1
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Please close all Internet Explorer windows (also ones running in the background), then click Retry." /SD IDCANCEL IDRETRY unCheckIE
    Abort
  ${EndIf}
FunctionEnd

Section "Uninstall"
  nsExec::Exec '"$SYSDIR\rundll32.exe" "$INSTDIR\bho\GeckoIEDoc.dll",GieCleanFavorites'
  Pop $0
  DetailPrint "Removing the native view and the Internet Explorer add-on..."
  nsExec::Exec '"$SYSDIR\regsvr32.exe" /u /s "$INSTDIR\bho\GeckoIEDoc.dll"'
  Pop $0
  DeleteRegKey HKLM "${BHO_KEY}"
  DeleteRegKey HKCR "CLSID\${BHO_CLSID}"
  DeleteRegKey HKCR "IEBridge.Bho"
  DetailPrint "Removing files..."
  RMDir /r "$INSTDIR\engine"
  RMDir /r "$INSTDIR\engine-removed"
  RMDir /r "$INSTDIR\engine-overlay"
  RMDir /r "$INSTDIR\bare-app"
  RMDir /r "$INSTDIR\bho"
  RMDir /r "$INSTDIR\bho-src"
  Delete "$INSTDIR\ieb.ini"
  Delete "$INSTDIR\restore-engine.bat"
  Delete "$INSTDIR\shell.html"
  Delete "$INSTDIR\classic-ie.cmd"
  Delete "$INSTDIR\set-ini.bat"
  Delete "$INSTDIR\setup-log.txt"
  Delete "$INSTDIR\omni-remove-browser.txt"
  Delete "$INSTDIR\gh-start.flag"
  Delete "$INSTDIR\ieb.ini.tmp"
  Delete "$INSTDIR\geckoie-bare-log.txt"
  Delete "$INSTDIR\omnislim.exe"
  Delete "$INSTDIR\omni-remove.txt"
  RMDir /r "$INSTDIR\bare-app-extra"
  RMDir /r "$PROFILE\AppData\LocalLow\GeckoIE"
  Delete "$INSTDIR\purge-cache.bat"
  Delete "$SMPROGRAMS\GeckoIE\Internet Explorer (classic).lnk"
  Delete "$SMPROGRAMS\GeckoIE\Uninstall GeckoIE.lnk"
  RMDir "$SMPROGRAMS\GeckoIE"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  RMDir /r "$APPDATA\GeckoIE"
  RMDir /r "$APPDATA\IEBridge"
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "Software\GeckoIE"
SectionEnd
