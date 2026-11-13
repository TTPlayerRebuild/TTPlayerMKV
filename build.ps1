param([string]$BuildDirectory = 'build', [string]$Version,
    [switch]$Package, [switch]$SourcePackage, [string[]]$CMakeArgs = @())
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) { [IO.Path]::GetFullPath($BuildDirectory) } else { [IO.Path]::GetFullPath((Join-Path $root $BuildDirectory)) }
$arguments = @('-S', $root, '-B', $build, '-G', 'Visual Studio 18 2026', '-A', 'Win32')
if ($Version) { $arguments += "-DTTP_MKV_BUILD_VERSION=$Version" }
& cmake @arguments @CMakeArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& cmake --build $build --config Release --target ttp_mkv --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Build or legacy import audit failed' }
$version = [IO.File]::ReadAllText((Join-Path $build 'BUILD_VERSION')).Trim()
if ($Package) {
    $stage = Join-Path $build "package-$version"
    New-Item -ItemType Directory -Force -Path (Join-Path $stage 'AddIn') | Out-Null
    $dll = Join-Path $build 'Release/ttp_mkv.dll'
    Copy-Item -LiteralPath $dll -Destination (Join-Path $stage 'AddIn/ttp_mkv.dll')
    $hash = (Get-FileHash $dll -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText((Join-Path $stage 'SHA256SUMS.txt'), "$hash  AddIn/ttp_mkv.dll`n", [Text.UTF8Encoding]::new($false))
    $zip = Join-Path $build "Release/ttp_mkv-$version.zip"
    & python (Join-Path $root 'cmake/package_runtime.py') --dll $dll --output $zip
    if ($LASTEXITCODE -ne 0) { throw 'Runtime packaging failed' }
}
if ($SourcePackage) {
    & python (Join-Path $root 'cmake/package_source.py') --root $root --build $build --version $version
    if ($LASTEXITCODE -ne 0) { throw 'Source packaging failed' }
}
if ($Package -or $SourcePackage) {
    $sums = Get-ChildItem (Join-Path $build "Release/ttp_mkv-$version*.zip") | Sort-Object Name | ForEach-Object {
        "$( (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() )  $($_.Name)"
    }
    [IO.File]::WriteAllText((Join-Path $build 'Release/SHA256SUMS.txt'), ($sums -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
}
