; QMud Project
; Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
;
; File: tools/scripts/wininstall.nsi
; Role: NSIS installer script for QMud portable payload deployment and CLI-driven update extraction.

!ifndef QMUD_SOURCE_DIR
  !error "QMUD_SOURCE_DIR define is required (directory containing staged QMud files)."
!endif

!ifndef QMUD_OUTPUT_FILE
  !define QMUD_OUTPUT_FILE "QMud-setup.exe"
!endif

!ifndef QMUD_VERSION
  !define QMUD_VERSION "dev"
!endif

!ifndef QMUD_INSTALLER_ICON
  !error "QMUD_INSTALLER_ICON define is required (path to QMud installer icon .ico)."
!endif

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!define MUI_ICON "${QMUD_INSTALLER_ICON}"

Unicode True
Target amd64-unicode
ManifestDPIAware True
Name "QMud ${QMUD_VERSION}"
OutFile "${QMUD_OUTPUT_FILE}"
Icon "${QMUD_INSTALLER_ICON}"
InstallDir "C:\QMud"
InstallDirRegKey HKCU "Software\QMud" "InstallDir"
RequestExecutionLevel admin
ShowInstDetails show

Var CliTargetDir
Var CliRunProcess
Var CliWaitPid
Var HasLegacyPrefsDb
Var HasLegacyIni

!define QMUD_REG_KEY "Software\QMud"

Function RejectUnsupportedInstallDir
  StrCpy $0 "$PROGRAMFILES"
  StrLen $1 $0
  StrCpy $2 "$INSTDIR" $1
  ${If} $2 == $0
    MessageBox MB_ICONEXCLAMATION|MB_OK "Installing QMud under Program Files is not supported. Use a folder like C:\QMud to avoid update issues."
    Abort
  ${EndIf}

  ${If} ${RunningX64}
    StrCpy $0 "$PROGRAMFILES64"
    StrLen $1 $0
    StrCpy $2 "$INSTDIR" $1
    ${If} $2 == $0
      MessageBox MB_ICONEXCLAMATION|MB_OK "Installing QMud under Program Files is not supported. Use a folder like C:\QMud to avoid update issues."
      Abort
    ${EndIf}
  ${EndIf}
FunctionEnd

Function WaitForProcessExitByPid
  ${If} $CliWaitPid == ""
    Return
  ${EndIf}

  StrCpy $0 $CliWaitPid
  IntOp $0 $0 + 0
  ${If} $0 <= 0
    Return
  ${EndIf}

  System::Call 'kernel32::OpenProcess(i 0x00100000, i 0, i r0) p.r1'
  ${If} $1 != 0
    DetailPrint "Waiting for running QMud process (PID $0) to exit..."
    System::Call 'kernel32::WaitForSingleObject(p r1, i -1) i.r2'
    System::Call 'kernel32::CloseHandle(p r1)'
  ${EndIf}
FunctionEnd

Function StripOptionalOuterQuotes
  Exch $0
  StrLen $1 $0
  ${If} $1 >= 2
    StrCpy $2 $0 1
    IntOp $3 $1 - 1
    StrCpy $4 $0 1 $3
    ${If} $2 == '"'
    ${AndIf} $4 == '"'
      IntOp $1 $1 - 2
      StrCpy $0 $0 $1 1
    ${EndIf}
  ${EndIf}
  Exch $0
FunctionEnd

Function .onInit
  ${GetParameters} $0
  ${GetOptions} "$0" "/TARGETDIR=" $CliTargetDir
  ${If} $CliTargetDir == ""
    ${GetOptions} "$0" "/targetdir=" $CliTargetDir
  ${EndIf}
  ${If} $CliTargetDir == ""
    ${GetOptions} "$0" "/EXTRACTDIR=" $CliTargetDir
  ${EndIf}
  ${If} $CliTargetDir == ""
    ${GetOptions} "$0" "/extractdir=" $CliTargetDir
  ${EndIf}
  ${If} $CliTargetDir != ""
    Push $CliTargetDir
    Call StripOptionalOuterQuotes
    Pop $CliTargetDir
  ${EndIf}
  ${If} $CliTargetDir != ""
    StrCpy $INSTDIR $CliTargetDir
  ${EndIf}

  ${GetOptions} "$0" "/RUNPROCESS=" $CliRunProcess
  ${If} $CliRunProcess == ""
    ${GetOptions} "$0" "/runprocess=" $CliRunProcess
  ${EndIf}
  ${If} $CliRunProcess == ""
    ${GetOptions} "$0" "/RUN=" $CliRunProcess
  ${EndIf}
  ${If} $CliRunProcess == ""
    ${GetOptions} "$0" "/run=" $CliRunProcess
  ${EndIf}
  ${If} $CliRunProcess != ""
    Push $CliRunProcess
    Call StripOptionalOuterQuotes
    Pop $CliRunProcess
  ${EndIf}

  ${GetOptions} "$0" "/WAITPID=" $CliWaitPid
  ${If} $CliWaitPid == ""
    ${GetOptions} "$0" "/waitpid=" $CliWaitPid
  ${EndIf}

  Call RejectUnsupportedInstallDir
FunctionEnd

Function .onVerifyInstDir
  Call RejectUnsupportedInstallDir
FunctionEnd

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR"
  Call WaitForProcessExitByPid

  StrCpy $HasLegacyPrefsDb 0
  IfFileExists "$INSTDIR\mushclient_prefs.sqlite" 0 +2
    StrCpy $HasLegacyPrefsDb 1
  StrCpy $HasLegacyIni 0
  IfFileExists "$INSTDIR\MUSHCLIENT.INI" 0 +2
    StrCpy $HasLegacyIni 1

  ; Pass 1: AppImage-style sync for normal payload files (copy if source is newer).
  ; Explicitly exclude files handled by policy-specific passes below.
  SetOverwrite ifnewer
  File /r /x "*.dll" /x "QMud.exe" /x "*.qdl" /x "*.db" /x "QMud.sqlite" /x "QMud.conf" "${QMUD_SOURCE_DIR}/*.*"
  File /nonfatal /oname=help.db "${QMUD_SOURCE_DIR}/help.db"

  ; Pass 2: World and non-help DB files are copied only when missing.
  SetOverwrite off
  File /nonfatal /r "${QMUD_SOURCE_DIR}/*.qdl"
  File /nonfatal /r /x "help.db" "${QMUD_SOURCE_DIR}/*.db"

  ; Pass 3: QMud.sqlite / QMud.conf follow legacy-gate + no-overwrite rules.
  ${If} $HasLegacyPrefsDb == 0
    IfFileExists "$INSTDIR\QMud.sqlite" +2 0
      File /oname=QMud.sqlite "${QMUD_SOURCE_DIR}/QMud.sqlite"
  ${EndIf}
  ${If} $HasLegacyIni == 0
    IfFileExists "$INSTDIR\QMud.conf" +2 0
      File /oname=QMud.conf "${QMUD_SOURCE_DIR}/QMud.conf"
  ${EndIf}

  ; Pass 4: User-requested exceptions for installer behavior.
  ; DLLs and executable are always replaced.
  SetOverwrite on
  File /oname=QMud.exe "${QMUD_SOURCE_DIR}/QMud.exe"
  File /nonfatal /r "${QMUD_SOURCE_DIR}/*.dll"

  WriteRegStr HKCU "${QMUD_REG_KEY}" "InstallDir" "$INSTDIR"
  IfFileExists "$DESKTOP\QMud.lnk" +2 0
    CreateShortCut "$DESKTOP\QMud.lnk" "$INSTDIR\QMud.exe" "" "$INSTDIR\QMud.exe" 0

  ${If} $CliRunProcess != ""
    Exec '"$CliRunProcess"'
  ${EndIf}
SectionEnd
