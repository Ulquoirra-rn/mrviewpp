; Inno Setup script for the mrview++ Windows installer.
;
; Prerequisites (build on Windows, e.g. via MSYS2 MinGW64 as per the MRtrix3
; Windows build docs):
;   - Build bin\mrview.exe and lib\libmrtrix.dll
;   - Stage a self-contained folder "dist\" containing mrview++.exe (renamed
;     from mrview.exe) plus every DLL it needs. The easiest way is:
;         mkdir dist
;         copy bin\mrview.exe dist\mrview++.exe
;         copy lib\libmrtrix.dll dist\
;         windeployqt --release --no-translations dist\mrview++.exe
;         copy the MSYS2 mingw64 DLLs it still reports missing (zlib, tiff,
;         png, jpeg, openjp2, gcc/stdc++/winpthread) into dist\
;   - Put mrview++.ico next to this script (convert icons/mrview++.png).
;
; Then build the installer:
;   iscc packaging\windows\mrviewpp.iss
;
; Produces: Output\mrview++-setup.exe

#define AppName "mrview++"
; The version comes from core/fork_version.h, passed in by make_installer.sh
; (or from the release tag by CI):  ISCC /DAppVer=3.0.5
; The fallback below is only reached if iscc is run by hand.
#ifndef AppVer
  #define AppVer "0.0.0"
#endif

[Setup]
; A fixed AppId lets Inno recognise an existing install and upgrade it IN PLACE
; (same folder, single Add/Remove entry) instead of installing side-by-side.
AppId={{4E544E81-4461-495E-9846-CE69A87FEB8A}
AppName={#AppName}
AppVersion={#AppVer}
VersionInfoVersion={#AppVer}
AppPublisher=BrainSight AI
DefaultDirName={autopf}\{#AppName}
UsePreviousAppDir=yes
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\mrview++.exe
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=mrview++-setup
SetupIconFile=mrviewpp.ico
; On upgrade, close a running mrview++ so its files can be replaced.
CloseApplications=yes
RestartApplications=no

[Files]
; Everything staged in dist\ (exe + all DLLs + Qt plugins) is installed as-is.
Source: "..\..\dist\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\{#AppName}";            Filename: "{app}\mrview++.exe"
Name: "{commondesktop}\{#AppName}";    Filename: "{app}\mrview++.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional icons:"

[Run]
Filename: "{app}\mrview++.exe"; Description: "Launch mrview++"; Flags: nowait postinstall skipifsilent
