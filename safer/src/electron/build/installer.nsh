!include "LogicLib.nsh"
!include "x64.nsh"

!define PS_SERVICE_NAME "PersonalSafer"
!define PS_SERVICE_KEY "SYSTEM\CurrentControlSet\Services\PersonalSafer"
!define PS_INSTANCE_NAME "PersonalSafer Instance"
!define PS_ALTITUDE "379950"

!macro PS_ABORT_DRIVER_STOP MESSAGE
  MessageBox MB_ICONSTOP|MB_OK "${MESSAGE}"
  Abort
!macroend

!macro PS_QUERY_FILTER RESULT_VAR
  nsExec::ExecToStack /TIMEOUT=10000 '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "$$o = (& $$env:SystemRoot\System32\fltmc.exe filters 2>&1 | Out-String); if ($$LASTEXITCODE -ne 0) { exit 2 }; if ($$o -match `"PersonalSafer`") { exit 0 }; exit 1"'
  Pop ${RESULT_VAR}
  Pop $1
!macroend

!macro PS_STOP_EXISTING_PRODUCT UNINSTALL_PREFIX
  ; Do not replace or remove a driver image until its filter is absent.
  !insertmacro PS_QUERY_FILTER $0
  StrCmp $0 "1" ps_stop_process_only
  StrCmp $0 "0" ps_filter_loaded
  !insertmacro PS_ABORT_DRIVER_STOP "Unable to determine the PersonalSafer driver state. Restart Windows and retry."

ps_filter_loaded:
  IfFileExists "$INSTDIR\PersonalSafer.exe" 0 ps_unlock_missing
  nsExec::ExecToStack /TIMEOUT=15000 '"$INSTDIR\PersonalSafer.exe" --ps-maintenance-action=unlock --ps-maintenance-out="$TEMP\PersonalSafer-maintenance"'
  Pop $0
  Pop $1
  StrCmp $0 "0" ps_stop_process
  !insertmacro PS_ABORT_DRIVER_STOP "The existing PersonalSafer driver could not be unlocked. Restart Windows and retry."

ps_unlock_missing:
  !insertmacro PS_ABORT_DRIVER_STOP "The driver is loaded but the maintenance executable is missing. Restart Windows and retry."

ps_stop_process:
  nsExec::ExecToLog /TIMEOUT=10000 '"$SYSDIR\taskkill.exe" /F /T /IM PersonalSafer.exe'
  nsExec::ExecToStack /TIMEOUT=20000 '"$SYSDIR\fltmc.exe" unload ${PS_SERVICE_NAME}'
  Pop $0
  Pop $1
  Sleep 500

  !insertmacro PS_QUERY_FILTER $0
  StrCmp $0 "1" ps_filter_absent
  !insertmacro PS_ABORT_DRIVER_STOP "The PersonalSafer MiniFilter did not unload. No driver files were changed. Restart Windows and retry."

ps_filter_absent:
  nsExec::ExecToLog /TIMEOUT=10000 '"$SYSDIR\sc.exe" stop ${PS_SERVICE_NAME}'

ps_stop_process_only:
  nsExec::ExecToLog /TIMEOUT=10000 '"$SYSDIR\taskkill.exe" /F /T /IM PersonalSafer.exe'
!macroend

!macro PS_SET_NATIVE_SYSTEM_CONTEXT
  ${If} ${RunningX64}
    SetRegView 64
    ${DisableX64FSRedirection}
  ${EndIf}
!macroend

!macro PS_RESTORE_SYSTEM_CONTEXT
  ${If} ${RunningX64}
    ${EnableX64FSRedirection}
  ${EndIf}
!macroend

!macro PS_WRITE_DRIVER_REGISTRY
  DeleteRegKey HKLM "${PS_SERVICE_KEY}"
  ; A missing key is expected on first install; only track the writes below.
  ClearErrors
  WriteRegStr HKLM "${PS_SERVICE_KEY}" "DisplayName" "PersonalSafer Security Minifilter"
  WriteRegStr HKLM "${PS_SERVICE_KEY}" "Description" "PersonalSafer file and network security minifilter"
  WriteRegDWORD HKLM "${PS_SERVICE_KEY}" "Type" 0x00000002
  WriteRegDWORD HKLM "${PS_SERVICE_KEY}" "Start" 0x00000003
  WriteRegDWORD HKLM "${PS_SERVICE_KEY}" "ErrorControl" 0x00000001
  WriteRegExpandStr HKLM "${PS_SERVICE_KEY}" "ImagePath" "\SystemRoot\System32\drivers\PersonalSafer.sys"
  WriteRegStr HKLM "${PS_SERVICE_KEY}" "Group" "FSFilter Activity Monitor"
  ; UTF-16LE REG_MULTI_SZ for "FltMgr" followed by the required double NUL.
  WriteRegMultiStr /REGEDIT5 HKLM "${PS_SERVICE_KEY}" "DependOnService" "46006c0074004d006700720000000000"

  WriteRegDWORD HKLM "${PS_SERVICE_KEY}\Parameters" "SupportedFeatures" 0x00000003
  WriteRegStr HKLM "${PS_SERVICE_KEY}\Parameters\Instances" "DefaultInstance" "${PS_INSTANCE_NAME}"
  WriteRegStr HKLM "${PS_SERVICE_KEY}\Parameters\Instances\${PS_INSTANCE_NAME}" "Altitude" "${PS_ALTITUDE}"
  WriteRegDWORD HKLM "${PS_SERVICE_KEY}\Parameters\Instances\${PS_INSTANCE_NAME}" "Flags" 0x00000000

  ; Keep the legacy location for Windows builds that still read Instances here.
  WriteRegStr HKLM "${PS_SERVICE_KEY}\Instances" "DefaultInstance" "${PS_INSTANCE_NAME}"
  WriteRegStr HKLM "${PS_SERVICE_KEY}\Instances\${PS_INSTANCE_NAME}" "Altitude" "${PS_ALTITUDE}"
  WriteRegDWORD HKLM "${PS_SERVICE_KEY}\Instances\${PS_INSTANCE_NAME}" "Flags" 0x00000000
!macroend

!macro customInit
  !insertmacro PS_STOP_EXISTING_PRODUCT ""
!macroend

!macro customInstall
  IfFileExists "$INSTDIR\resources\driver\PersonalSafer.sys" 0 ps_driver_source_missing

  !insertmacro PS_SET_NATIVE_SYSTEM_CONTEXT
  ClearErrors
  CreateDirectory "$SYSDIR\drivers"
  CopyFiles /SILENT "$INSTDIR\resources\driver\PersonalSafer.sys" "$SYSDIR\drivers\PersonalSafer.sys"
  IfErrors ps_driver_copy_failed

  ClearErrors
  !insertmacro PS_WRITE_DRIVER_REGISTRY
  IfErrors ps_driver_registry_failed

  ReadRegDWORD $0 HKLM "${PS_SERVICE_KEY}" "Type"
  StrCmp $0 "2" ps_driver_install_complete ps_driver_registry_failed

ps_driver_source_missing:
  MessageBox MB_ICONSTOP|MB_OK "The packaged driver is missing. Installation cannot continue."
  Abort

ps_driver_copy_failed:
  !insertmacro PS_RESTORE_SYSTEM_CONTEXT
  MessageBox MB_ICONSTOP|MB_OK "PersonalSafer.sys could not be copied to the Windows drivers directory."
  Abort

ps_driver_registry_failed:
  DeleteRegKey HKLM "${PS_SERVICE_KEY}"
  Delete /REBOOTOK "$SYSDIR\drivers\PersonalSafer.sys"
  !insertmacro PS_RESTORE_SYSTEM_CONTEXT
  MessageBox MB_ICONSTOP|MB_OK "The PersonalSafer driver service registry could not be created."
  Abort

ps_driver_install_complete:
  !insertmacro PS_RESTORE_SYSTEM_CONTEXT
!macroend

!macro customUnInit
  !insertmacro PS_STOP_EXISTING_PRODUCT "un."
!macroend

!macro customUnInstall
  !insertmacro PS_SET_NATIVE_SYSTEM_CONTEXT
  DeleteRegKey HKLM "${PS_SERVICE_KEY}"
  Delete /REBOOTOK "$SYSDIR\drivers\PersonalSafer.sys"
  !insertmacro PS_RESTORE_SYSTEM_CONTEXT
!macroend
