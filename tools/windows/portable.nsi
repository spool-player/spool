Unicode true

!ifndef VERSION
    !error "VERSION is required"
!endif
!ifndef STAGE_DIR
    !error "STAGE_DIR is required"
!endif
!ifndef PAYLOAD_ID
    !error "PAYLOAD_ID is required"
!endif
!ifndef SOURCE_ROOT
    !error "SOURCE_ROOT is required"
!endif
!ifndef OUTPUT_FILE
    !error "OUTPUT_FILE is required"
!endif

Name "Spool Portable"
OutFile "${OUTPUT_FILE}"
Icon "${SOURCE_ROOT}\app\icons\spool.ico"
RequestExecutionLevel user
SilentInstall silent
AutoCloseWindow true
ManifestDPIAware true
SetCompressor /SOLID lzma
InstallDir "$LocalAppData\spool-jellyfin\portable\${VERSION}-${PAYLOAD_ID}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "Spool Portable"
VIAddVersionKey /LANG=1033 "FileDescription" "Spool portable launcher"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Spool contributors"

Section
    IfFileExists "$InstDir\.payload-complete" launch

    SetOutPath "$InstDir"
    File /r "${STAGE_DIR}\*"

    FileOpen $0 "$InstDir\.payload-complete" w
    FileWrite $0 "${PAYLOAD_ID}"
    FileClose $0

launch:
    ClearErrors
    Exec '"$InstDir\spool.exe"'
    IfErrors launch_failed
    Call PruneOldPayloads
    Goto done

launch_failed:
    MessageBox MB_ICONSTOP "Spool could not be launched."
    SetErrorLevel 1

done:
SectionEnd

Function PruneOldPayloads
    ; The application is already running. Keep this silent launcher alive in
    ; the background briefly, then remove every obsolete extracted payload.
    Sleep 10000
    FindFirst $0 $1 "$LocalAppData\spool-jellyfin\portable\*"

prune_loop:
    StrCmp $1 "" prune_done
    StrCmp $1 "." prune_next
    StrCmp $1 ".." prune_next
    StrCmp $1 "${VERSION}-${PAYLOAD_ID}" prune_next
    RMDir /r "$LocalAppData\spool-jellyfin\portable\$1"

prune_next:
    FindNext $0 $1
    Goto prune_loop

prune_done:
    FindClose $0
FunctionEnd
