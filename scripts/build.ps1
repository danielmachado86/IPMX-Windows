[CmdletBinding()]
param(
    [Parameter()]
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "RelWithDebInfo",

    [Parameter()]
    [string]$BuildDirectory = "out/build/windows-msvc",

    [Parameter()]
    [switch]$WarningsAsErrors,

    [Parameter()]
    [switch]$SkipTests,

    [Parameter()]
    [ValidatePattern("^[A-Z]$")]
    [string]$ShortDrive = "P"
)

$ErrorActionPreference = "Stop"
$sourceRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Installer (vswhere.exe) was not found."
}

$installationPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $installationPath) {
    throw "Visual Studio with the MSVC x64 toolset was not found."
}

$developerCommand = Join-Path $installationPath "Common7\Tools\VsDevCmd.bat"
$environmentLines = & cmd.exe /s /c "`"$developerCommand`" -arch=x64 -host_arch=x64 >nul && set"
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf("=")
    if ($separator -gt 0) {
        [Environment]::SetEnvironmentVariable(
            $line.Substring(0, $separator),
            $line.Substring($separator + 1),
            "Process"
        )
    }
}

$vcpkgRoot = $env:VCPKG_ROOT
if (-not $vcpkgRoot) {
    $bundledVcpkg = Join-Path $installationPath "VC\vcpkg"
    if (Test-Path -LiteralPath (Join-Path $bundledVcpkg "scripts\buildsystems\vcpkg.cmake")) {
        $vcpkgRoot = $bundledVcpkg
    }
}
if (-not $vcpkgRoot) {
    throw "Set VCPKG_ROOT or install the Visual Studio vcpkg component."
}

$toolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path -LiteralPath $toolchain)) {
    throw "The vcpkg toolchain was not found at '$toolchain'."
}

$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
$ctestCommand = Get-Command ctest.exe -ErrorAction SilentlyContinue
if ($cmakeCommand -and $ctestCommand) {
    $cmake = $cmakeCommand.Source
    $ctest = $ctestCommand.Source
} else {
    $cmakeBin = Join-Path $installationPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
    $cmake = Join-Path $cmakeBin "cmake.exe"
    $ctest = Join-Path $cmakeBin "ctest.exe"
    if (-not (Test-Path -LiteralPath $cmake) -or -not (Test-Path -LiteralPath $ctest)) {
        throw "CMake and CTest were not found in PATH or the Visual Studio installation."
    }
}

$ninjaDirectory = Join-Path $installationPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$ninja = Join-Path $ninjaDirectory "ninja.exe"
if (Test-Path -LiteralPath $ninja) {
    $env:Path = "$ninjaDirectory;$env:Path"
} else {
    throw "Ninja was not found in the Visual Studio installation."
}

# Keep the registry cache project-local. This also avoids a stale global vcpkg registry lock from
# making otherwise reproducible manifest installs fail.
$env:X_VCPKG_REGISTRIES_CACHE = Join-Path $sourceRoot "out\vcpkg-registries"
New-Item -ItemType Directory -Path $env:X_VCPKG_REGISTRIES_CACHE -Force | Out-Null
$sourceForBuild = $sourceRoot
$createdMapping = $false
$drive = "$ShortDrive`:"

try {
    if ($sourceRoot.Contains(" ")) {
        $existingMapping = (& subst.exe) | Where-Object { $_ -like "$drive\:*" }
        if ($existingMapping) {
            $mappedPath = ($existingMapping -split "=>", 2)[1].Trim()
            if (-not $mappedPath.Equals($sourceRoot, [StringComparison]::OrdinalIgnoreCase)) {
                throw "$drive is already mapped to '$mappedPath'. Choose another -ShortDrive."
            }
        } else {
            & subst.exe $drive $sourceRoot
            if ($LASTEXITCODE -ne 0) { throw "Could not map $drive to the source directory." }
            $createdMapping = $true
        }
        $sourceForBuild = "$drive\"
    }

    $buildPath = Join-Path $sourceForBuild $BuildDirectory
    $configureArguments = @(
        "-S", $sourceForBuild,
        "-B", $buildPath,
        "-G", "Ninja",
        "-DCMAKE_MAKE_PROGRAM=$ninja",
        "-DCMAKE_BUILD_TYPE=$Configuration",
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
        "-DVCPKG_TARGET_TRIPLET=x64-windows",
        "-DVCPKG_APPLOCAL_DEPS=OFF",
        "-DBUILD_TESTING=ON",
        "-DIPMX_WARNINGS_AS_ERRORS=$($WarningsAsErrors.IsPresent.ToString().ToUpperInvariant())"
    )
    & $cmake @configureArguments
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE." }

    & $cmake --build $buildPath --parallel 1
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed with exit code $LASTEXITCODE." }

    if (-not $SkipTests) {
        & $ctest --test-dir $buildPath --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "CTest failed with exit code $LASTEXITCODE." }
    }
}
finally {
    if ($createdMapping) {
        & subst.exe $drive /D
    }
}
