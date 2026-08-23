; ============================================================================
;  AZ Soundtech Hands-free -- Windows installer  (plan Task 30)
;
;  Build:   makensis installer\handsfree.nsi
;  Output:  installer\AZSoundtech-Handsfree-Setup-<version>.exe
;
;  Two prerequisites, both of which fail loudly rather than silently:
;    1. The app must be built.        -> ${BUILD_DIR} below
;    2. installer\fetch-deps.ps1 must have been run once, to place
;       vendor\vc_redist.x64.exe.    -> see "Visual C++ runtime" below
;
;  See README.md beside this file.
; ============================================================================

Unicode true
SetCompressor /SOLID lzma

; ---------------------------------------------------------------------------
;  Version -- single source of truth
;
;  There is exactly ONE authoritative version in this project: the
;  project(HandsFree VERSION x.y.z) line in CMakeLists.txt. Rather than add a
;  third copy of it here (CMakeLists already carries two), read it at compile
;  time. PRODUCT_VERSION, the output filename and the Apps & Features entry
;  below all derive from this one value, so they cannot drift apart.
;
;  If that project() line is ever reworded, this !searchparse FAILS THE BUILD.
;  That is deliberate -- an installer stamped with a guessed version is worse
;  than an installer that refuses to compile.
; ---------------------------------------------------------------------------
!ifndef PRODUCT_VERSION
  !searchparse /file "..\CMakeLists.txt" `project(HandsFree VERSION ` PRODUCT_VERSION `)`
!endif

!define PRODUCT_NAME      "AZ Soundtech Hands-free"
!define PRODUCT_PUBLISHER "AZ Soundtech"
!define PRODUCT_WEB       "https://azsoundtech.com"
!define REG_KEY_NAME      "AZSoundtechHandsFree"
!define UNINST_KEY        "Software\Microsoft\Windows\CurrentVersion\Uninstall\${REG_KEY_NAME}"

; ---------------------------------------------------------------------------
;  Paths into the build tree
;
;  NOT "..\build\Release\HandsFree.exe". Three separate things are wrong with
;  that path, and all three were verified against a real build:
;    * juce_add_gui_app emits into <build>/HandsFree_artefacts/Release/,
;      never into <build>/Release/.
;    * The file is named after PRODUCT_NAME ("AZ Soundtech Hands-free"), not
;      after the CMake target name ("HandsFree"). Those are different things.
;    * The real name contains spaces, so every File / CreateShortcut / Delete
;      line that mentions it must be quoted, or NSIS takes "AZ" as the whole
;      argument and chokes on the rest.
; ---------------------------------------------------------------------------
; All relative paths in this script -- BUILD_DIR, VCREDIST_SRC, OutFile, the
; !searchparse above -- resolve against THIS FILE'S directory, not against the
; shell's working directory. Verified with a probe script: makensis was invoked
; from the repo root and OutFile still landed in installer\. That is why none
; of these use ${__FILEDIR__}; prefixing it doubles the path.
!ifndef BUILD_DIR
  !define BUILD_DIR "..\build"
!endif

!define APP_EXE      "${PRODUCT_NAME}.exe"
!define APP_EXE_SRC  "${BUILD_DIR}\HandsFree_artefacts\Release\${APP_EXE}"
!define VCREDIST_SRC "vendor\vc_redist.x64.exe"

; Fail at compile time, with a readable message, rather than emitting an
; installer that is quietly missing its payload.
!if /FileExists "${APP_EXE_SRC}"
!else
  !error "App not built. Expected: ${APP_EXE_SRC} -- build it first, or pass /DBUILD_DIR=<your build dir> to makensis."
!endif
!if /FileExists "${VCREDIST_SRC}"
!else
  !error "Missing ${VCREDIST_SRC} -- run installer\fetch-deps.ps1 once to download it (~25 MB)."
!endif

Name    "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "AZSoundtech-Handsfree-Setup-${PRODUCT_VERSION}.exe"
BrandingText "${PRODUCT_PUBLISHER}"

; $PROGRAMFILES64 needs elevation. Without this line the write is redirected
; into the per-user VirtualStore on some configurations: the install appears
; to succeed, and the all-users shortcut then points at a path that does not
; exist for anybody else.
RequestExecutionLevel admin
InstallDir "$PROGRAMFILES64\AZSoundtech\HandsFree"
InstallDirRegKey HKLM "Software\${PRODUCT_PUBLISHER}\HandsFree" "InstallDir"

VIProductVersion "${PRODUCT_VERSION}.0"
VIAddVersionKey "ProductName"     "${PRODUCT_NAME}"
VIAddVersionKey "ProductVersion"  "${PRODUCT_VERSION}"
VIAddVersionKey "FileVersion"     "${PRODUCT_VERSION}.0"
VIAddVersionKey "CompanyName"     "${PRODUCT_PUBLISHER}"
VIAddVersionKey "LegalCopyright"  "(c) ${PRODUCT_PUBLISHER}"
VIAddVersionKey "FileDescription" "${PRODUCT_NAME} Setup"

!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${PRODUCT_NAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------------
;  Install
; ---------------------------------------------------------------------------

Function .onInit
  ; The app is x64 only -- built with -A x64, linking the x64 runtime. Say so
  ; here rather than letting it fail at first launch.
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "${PRODUCT_NAME} requires 64-bit Windows."
    Abort
  ${EndIf}

  ; NSIS runs as a 32-bit process, so bare HKLM reads and writes are silently
  ; redirected into WOW6432Node. Everything this installer records must live
  ; in the 64-bit view, or 64-bit Apps & Features will never show it.
  SetRegView 64

  ; Offer to remove a previous install first, so two Apps & Features entries
  ; can never accumulate.
  ReadRegStr $R0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${REG_KEY_NAME}" "UninstallString"
  ${If} $R0 != ""
  ${AndIfNot} ${Silent}
    MessageBox MB_YESNO|MB_ICONQUESTION "${PRODUCT_NAME} is already installed.$\n$\nRemove the previous version first?" IDNO +2
    ExecWait '$R0 /S _?=$INSTDIR'
  ${EndIf}
FunctionEnd

Section "Application" SEC_APP
  SectionIn RO
  SetRegView 64
  SetShellVarContext all

  SetOutPath "$INSTDIR"
  File "${APP_EXE_SRC}"

  ; --- Visual C++ runtime -------------------------------------------------
  ;
  ;  Why this is here at all: the shipped binary really does import
  ;  MSVCP140.dll, VCRUNTIME140.dll and VCRUNTIME140_1.dll. That was read out
  ;  of the built exe's import table, not assumed. Those three ship in the
  ;  VC++ 2015-2022 redistributable and are NOT part of Windows. (The
  ;  api-ms-win-crt-*.dll imports ARE part of Windows 10+ and need nothing.)
  ;
  ;  Without this step the first launch on a clean machine dies with a
  ;  VCRUNTIME140.dll dialog, which a user cannot tell apart from a product
  ;  that simply does not work.
  ;
  ;  Skipped when a new enough runtime is already present, which is the common
  ;  case, so most installs never pay the ~10 s.
  DetailPrint "Checking Visual C++ runtime..."
  ClearErrors
  ReadRegDWORD $0 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\X64" "Installed"
  ReadRegDWORD $1 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\X64" "Major"
  ReadRegDWORD $2 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\X64" "Minor"

  ; Require >= 14.29. VCRUNTIME140_1.dll did not exist in the earliest 14.x
  ; releases, so "Installed=1" on its own is not enough. Being conservative
  ; here only costs a redundant redistributable run, which is harmless.
  ${If} ${Errors}
  ${OrIf} $0 != 1
  ${OrIf} $1 < 14
  ${OrIf} $2 < 29
    DetailPrint "Installing Visual C++ runtime (required)..."
    SetOutPath "$PLUGINSDIR"
    File "${VCREDIST_SRC}"
    ExecWait '"$PLUGINSDIR\vc_redist.x64.exe" /install /quiet /norestart' $3
    SetOutPath "$INSTDIR"

    ; 0 = installed, 1638 = a newer one is already there, 3010 = installed but
    ; wants a reboot. Anything else is a real failure and must not be
    ; swallowed silently.
    ${If} $3 == 0
      DetailPrint "Visual C++ runtime installed."
    ${ElseIf} $3 == 1638
      DetailPrint "A newer Visual C++ runtime is already present."
    ${ElseIf} $3 == 3010
      DetailPrint "Visual C++ runtime installed; a reboot is pending."
      SetRebootFlag true
    ${Else}
      DetailPrint "Visual C++ runtime FAILED with exit code $3."
      ${IfNot} ${Silent}
        MessageBox MB_OK|MB_ICONEXCLAMATION "The Visual C++ runtime could not be installed (code $3).$\n$\n${PRODUCT_NAME} will not start until it is present. Install it from:$\nhttps://aka.ms/vs/17/release/vc_redist.x64.exe"
      ${EndIf}
    ${EndIf}
  ${Else}
    DetailPrint "Visual C++ runtime already present ($1.$2) -- skipping."
  ${EndIf}

  ; --- Shortcuts ----------------------------------------------------------
  ; Every argument quoted. The product name contains spaces.
  CreateDirectory "$SMPROGRAMS\${PRODUCT_PUBLISHER}"
  CreateShortcut "$SMPROGRAMS\${PRODUCT_PUBLISHER}\${PRODUCT_NAME}.lnk" "$INSTDIR\${APP_EXE}"
  CreateShortcut "$DESKTOP\${PRODUCT_NAME}.lnk" "$INSTDIR\${APP_EXE}"

  ; --- Uninstaller and Apps & Features ------------------------------------
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\${PRODUCT_PUBLISHER}\Uninstall ${PRODUCT_NAME}.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKLM "Software\${PRODUCT_PUBLISHER}\HandsFree" "InstallDir" "$INSTDIR"

  WriteRegStr   HKLM "${UNINST_KEY}" "DisplayName"          "${PRODUCT_NAME}"
  WriteRegStr   HKLM "${UNINST_KEY}" "DisplayVersion"       "${PRODUCT_VERSION}"
  WriteRegStr   HKLM "${UNINST_KEY}" "Publisher"            "${PRODUCT_PUBLISHER}"
  WriteRegStr   HKLM "${UNINST_KEY}" "DisplayIcon"          '"$INSTDIR\${APP_EXE}"'
  WriteRegStr   HKLM "${UNINST_KEY}" "InstallLocation"      "$INSTDIR"
  WriteRegStr   HKLM "${UNINST_KEY}" "URLInfoAbout"         "${PRODUCT_WEB}"
  WriteRegStr   HKLM "${UNINST_KEY}" "UninstallString"      '"$INSTDIR\Uninstall.exe"'
  WriteRegStr   HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

; ---------------------------------------------------------------------------
;  Uninstall
; ---------------------------------------------------------------------------

Section "Uninstall"
  SetRegView 64
  SetShellVarContext all

  ; The app exe is the one file that can be locked at uninstall time: when the
  ; user uninstalls without closing the app first, or just after closing it,
  ; while Windows still holds the image.
  ;
  ; NSIS's Delete fails SILENTLY -- no error, no abort, execution simply
  ; continues. Left alone, this section would then go on to delete the registry
  ; key and the shortcuts and report success, leaving the binary and its folder
  ; in Program Files with the Apps & Features entry that would let the user
  ; retry ALREADY GONE. There is no route back through the UI from there.
  ;
  ; Observed, not theorised: installer\verify-installer.ps1 caught exactly this
  ; because it uninstalls immediately after killing the app, which is ruder
  ; than a human tester would ever be.
  retry_delete_app:
  ClearErrors
  Delete "$INSTDIR\${APP_EXE}"
  IfErrors 0 app_deleted
    IfSilent app_delete_reboot
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Cannot remove:$\n$INSTDIR\${APP_EXE}$\n$\nIt is still running. Close ${PRODUCT_NAME} and click Retry." IDRETRY retry_delete_app
    app_delete_reboot:
    ; Last resort. Hand it to the OS to remove on the next reboot rather than
    ; orphaning it in Program Files forever.
    Delete /REBOOTOK "$INSTDIR\${APP_EXE}"
  app_deleted:

  Delete "$INSTDIR\Uninstall.exe"
  RMDir /REBOOTOK "$INSTDIR"
  RMDir /REBOOTOK "$PROGRAMFILES64\AZSoundtech"

  Delete "$SMPROGRAMS\${PRODUCT_PUBLISHER}\${PRODUCT_NAME}.lnk"
  Delete "$SMPROGRAMS\${PRODUCT_PUBLISHER}\Uninstall ${PRODUCT_NAME}.lnk"
  RMDir  "$SMPROGRAMS\${PRODUCT_PUBLISHER}"
  Delete "$DESKTOP\${PRODUCT_NAME}.lnk"

  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "Software\${PRODUCT_PUBLISHER}\HandsFree"
  DeleteRegKey /ifempty HKLM "Software\${PRODUCT_PUBLISHER}"

  ; --- User data ----------------------------------------------------------
  ;
  ;  %APPDATA%\AZSoundtech\HandsFree holds presets and the licence, and those
  ;  belong to the user rather than to this installer. Ask, defaulting to
  ;  keeping them, and never delete them during a SILENT uninstall -- a silent
  ;  uninstall is what an upgrade runs, and an upgrade must not eat a licence.
  ;
  ;  KNOWN LIMITATION: the uninstaller is elevated, so $APPDATA resolves to
  ;  the profile of whoever answered the UAC prompt. When an administrator
  ;  uninstalls on behalf of a different user, that user's data is left in
  ;  place. Leaving data behind is the safe direction to be wrong in.
  SetShellVarContext current
  ${IfNot} ${Silent}
    ${If} ${FileExists} "$APPDATA\AZSoundtech\HandsFree\*.*"
      MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "Also delete your presets and licence?$\n$\n$APPDATA\AZSoundtech\HandsFree$\n$\nChoose No to keep them for a future reinstall." IDNO skip_userdata
      RMDir /r "$APPDATA\AZSoundtech\HandsFree"
      RMDir "$APPDATA\AZSoundtech"
      skip_userdata:
    ${EndIf}
  ${EndIf}
SectionEnd

; ---------------------------------------------------------------------------
;  Task 31 -- code signing. NOT ENABLED. Do not simply uncomment this.
;
;  It is left unexecuted on purpose, blocked on two things that are not code:
;
;    1. No certificate has been purchased. Clearing SmartScreen needs an EV
;       (or OV plus accumulated reputation) code-signing certificate, roughly
;       $300-500/year. That is a business decision, not an engineering one.
;    2. signtool.exe is not installed on this build machine either. It ships
;       with the Windows SDK.
;
;  Do NOT substitute a self-signed certificate. A self-signed binary produces
;  the identical SmartScreen warning that Task 31 exists to remove, so it
;  would look finished while achieving exactly nothing.
;
;  Note the ordering: the app exe must be signed BEFORE makensis runs (it gets
;  embedded), and the installer signed AFTER. Signing is therefore a release
;  step the owner runs around makensis, not something makensis can do itself.
;
;    signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 ^
;      /f "C:\path\to\azsoundtech.pfx" /p <password> ^
;      "build\HandsFree_artefacts\Release\AZ Soundtech Hands-free.exe"
;
;    makensis installer\handsfree.nsi
;
;    signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 ^
;      /f "C:\path\to\azsoundtech.pfx" /p <password> ^
;      "installer\AZSoundtech-Handsfree-Setup-${PRODUCT_VERSION}.exe"
;
;    signtool verify /pa /v "installer\AZSoundtech-Handsfree-Setup-${PRODUCT_VERSION}.exe"
;
;  /tr (RFC-3161 timestamping) is not optional. Without it every signature
;  becomes invalid the day the certificate expires, including on copies users
;  have already downloaded.
; ---------------------------------------------------------------------------
