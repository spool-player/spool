Unicode true

!include "MUI2.nsh"

!ifndef VERSION
    !error "VERSION is required"
!endif
!ifndef STAGE_DIR
    !error "STAGE_DIR is required"
!endif
!ifndef SOURCE_ROOT
    !error "SOURCE_ROOT is required"
!endif
!ifndef OUTPUT_FILE
    !error "OUTPUT_FILE is required"
!endif

Name "Spool"
OutFile "${OUTPUT_FILE}"
Icon "${SOURCE_ROOT}\app\icons\spool.ico"
UninstallIcon "${SOURCE_ROOT}\app\icons\spool.ico"
InstallDir "$LocalAppData\Programs\Spool"
InstallDirRegKey HKCU "Software\Spool" "InstallDir"
RequestExecutionLevel user
ManifestDPIAware true
SetCompressor /SOLID lzma
ShowInstDetails nevershow
ShowUninstDetails nevershow
BrandingText "Spool"

VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "Spool"
VIAddVersionKey /LANG=1033 "FileDescription" "Spool installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Spool contributors"

!define MUI_ABORTWARNING
!define MUI_ICON "${SOURCE_ROOT}\app\icons\spool.ico"
!define MUI_UNICON "${SOURCE_ROOT}\app\icons\spool.ico"
!define MUI_FINISHPAGE_RUN "$InstDir\spool.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch Spool"

!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Spool" SEC_APP
    SectionIn RO
    SetShellVarContext current
    SetOutPath "$InstDir"
    File /r "${STAGE_DIR}\*"

    WriteUninstaller "$InstDir\Uninstall.exe"
    CreateDirectory "$SMPROGRAMS\Spool"
    CreateShortcut "$SMPROGRAMS\Spool\Spool.lnk" "$InstDir\spool.exe"
    CreateShortcut "$SMPROGRAMS\Spool\Uninstall Spool.lnk" "$InstDir\Uninstall.exe"

    WriteRegStr HKCU "Software\Spool" "InstallDir" "$InstDir"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Spool" \
        "DisplayName" "Spool"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Spool" \
        "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Spool" \
        "DisplayIcon" "$InstDir\spool.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Spool" \
        "InstallLocation" "$InstDir"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Spool" \
        "UninstallString" '"$InstDir\Uninstall.exe"'
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Spool" \
        "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Spool" \
        "NoRepair" 1
SectionEnd

Section "Uninstall"
    SetShellVarContext current
    Delete "$SMPROGRAMS\Spool\Spool.lnk"
    Delete "$SMPROGRAMS\Spool\Uninstall Spool.lnk"
    RMDir "$SMPROGRAMS\Spool"

    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Spool"
    DeleteRegKey HKCU "Software\Spool"

    Delete "$InstDir\Uninstall.exe"
    RMDir /r "$InstDir"
SectionEnd
