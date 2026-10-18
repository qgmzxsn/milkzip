Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!ifndef VERSION
  !error "VERSION is required"
!endif
Name "MilkZip ${VERSION}"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\MilkZip"
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 32
BrandingText "MilkZip"
Icon "${APP_ICON}"
UninstallIcon "${APP_ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\milkzip.exe"
!define MUI_FINISHPAGE_RUN_TEXT "启动 MilkZip"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "SimpChinese"

Var Integrate
Function .onInit
  StrCpy $Integrate "1"
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/NOINTEGRATION" $1
  ${IfNot} ${Errors}
    StrCpy $Integrate "0"
  ${EndIf}
FunctionEnd

Section "MilkZip" Main
  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteINIStr "$INSTDIR\install-state.ini" "Setup" "Integration" "$Integrate"
  ${If} $Integrate == "1"
    SetShellVarContext current
    CreateDirectory "$SMPROGRAMS\MilkZip"
    CreateShortCut "$SMPROGRAMS\MilkZip\MilkZip.lnk" "$INSTDIR\milkzip.exe"
    CreateShortCut "$SMPROGRAMS\MilkZip\卸载.lnk" "$INSTDIR\uninstall.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MilkZip" "DisplayName" "MilkZip"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MilkZip" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MilkZip" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MilkZip" "DisplayIcon" "$INSTDIR\milkzip.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MilkZip" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MilkZip" "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MilkZip" "NoRepair" 1
  ${EndIf}
SectionEnd

Section "Uninstall"
  ReadINIStr $Integrate "$INSTDIR\install-state.ini" "Setup" "Integration"
  ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MilkZip" "InstallLocation"
  ${If} $Integrate == "1"
  ${AndIf} $0 == $INSTDIR
    SetShellVarContext current
    Delete "$SMPROGRAMS\MilkZip\MilkZip.lnk"
    Delete "$SMPROGRAMS\MilkZip\卸载.lnk"
    RMDir "$SMPROGRAMS\MilkZip"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\MilkZip"
  ${EndIf}
  !include "${UNINSTALL_LIST}"
  Delete "$INSTDIR\install-state.ini"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
