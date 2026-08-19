!define AXKDECK_WEBVIEW2_APP_GUID "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"
!define AXKDECK_MINIMUM_WEBVIEW2_VERSION "111.0.0.0"

; Tauri's WebView2 section runs before NSIS_HOOK_PREINSTALL. Use the GUI
; callback so an interactive user can decline before any runtime is changed.
Function .onGUIInit
  IfSilent axkdeck_webview2_consent_done 0

  StrCpy $0 ""
  StrCpy $1 ""
  ${If} ${RunningX64}
    ReadRegStr $0 HKLM "SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\${AXKDECK_WEBVIEW2_APP_GUID}" "pv"
  ${Else}
    ReadRegStr $0 HKLM "SOFTWARE\Microsoft\EdgeUpdate\Clients\${AXKDECK_WEBVIEW2_APP_GUID}" "pv"
  ${EndIf}
  ReadRegStr $1 HKCU "SOFTWARE\Microsoft\EdgeUpdate\Clients\${AXKDECK_WEBVIEW2_APP_GUID}" "pv"

  ; Use the newest visible machine- or user-scoped Evergreen Runtime.
  StrCpy $2 $0
  ${If} $2 == ""
  ${OrIf} $2 == "0.0.0.0"
    StrCpy $2 $1
  ${Else}
    ${If} $1 != ""
      ${If} $1 != "0.0.0.0"
        ${VersionCompare} "$1" "$2" $3
        ${If} $3 == 1
          StrCpy $2 $1
        ${EndIf}
      ${EndIf}
    ${EndIf}
  ${EndIf}

  ${If} $2 == ""
  ${OrIf} $2 == "0.0.0.0"
    StrCpy $4 "Not installed"
    Goto axkdeck_webview2_consent_prompt
  ${EndIf}

  ${VersionCompare} "${AXKDECK_MINIMUM_WEBVIEW2_VERSION}" "$2" $3
  ${If} $3 != 1
    Goto axkdeck_webview2_consent_done
  ${EndIf}
  StrCpy $4 $2

  axkdeck_webview2_consent_prompt:
    MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION \
      "axkdeck requires the Microsoft Edge WebView2 Evergreen Runtime.$\r$\n$\r$\nInstalled version: $4$\r$\nMinimum required version: ${AXKDECK_MINIMUM_WEBVIEW2_VERSION}$\r$\n$\r$\nContinue to let setup download and install or update the shared runtime from Microsoft? WebView2 updates automatically after installation." \
      IDOK axkdeck_webview2_consent_done
    Quit

  axkdeck_webview2_consent_done:
FunctionEnd
