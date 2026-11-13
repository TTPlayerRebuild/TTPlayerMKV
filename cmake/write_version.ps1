param([Parameter(Mandatory)][string]$Output, [string]$Version)
$ErrorActionPreference = 'Stop'
if (-not $Version) { $Version = [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8)).ToString('yyyy.MM.dd') }
if ($Version -notmatch '^(\d{4})\.(\d{2})\.(\d{2})(?:p(\d+))?$') { throw 'Invalid date version' }
$numbers = @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], $(if ($Matches[4]) { [int]$Matches[4] } else { 0 })) -join ','
$content = @"
#include <windows.h>
1 VERSIONINFO
 FILEVERSION $numbers
 PRODUCTVERSION $numbers
 FILEFLAGSMASK 0x3fL
 FILEFLAGS 0
 FILEOS VOS_NT_WINDOWS32
 FILETYPE VFT_DLL
BEGIN
 BLOCK "StringFileInfo"
 BEGIN
  BLOCK "040904b0"
  BEGIN
   VALUE "CompanyName", "Sonic853\0"
   VALUE "FileDescription", "TTPlayer Matroska Audio Plugin\0"
   VALUE "FileVersion", "$Version\0"
   VALUE "InternalName", "ttp_mkv\0"
   VALUE "OriginalFilename", "ttp_mkv.dll\0"
   VALUE "ProductName", "TTPlayer Matroska Audio Plugin\0"
   VALUE "ProductVersion", "$Version\0"
   VALUE "LegalCopyright", "Copyright (c) 2026 Sonic853; FFmpeg contributors\0"
  END
 END
 BLOCK "VarFileInfo"
 BEGIN
  VALUE "Translation", 0x409, 1200
 END
END
"@
New-Item -ItemType Directory -Force -Path (Split-Path $Output) | Out-Null
if (-not (Test-Path $Output) -or [IO.File]::ReadAllText($Output) -cne $content) {
    [IO.File]::WriteAllText($Output, $content, [Text.UTF8Encoding]::new($false))
}
[IO.File]::WriteAllText((Join-Path (Split-Path (Split-Path $Output)) 'BUILD_VERSION'), $Version)
