param(
    [ValidateSet("configure", "build", "ensure", "all")]
    [string]$Command = "all",
    [string[]]$CMakeArgs = @()
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = if ($env:BUILD_DIR) { $env:BUILD_DIR } else {
    Join-Path $Root "build/mariadb-embedded"
}
$Profile = if ($env:PROFILE) { $env:PROFILE } else {
    Join-Path $Root "cmake/mariadb-embedded-windows.cmake"
}

function Configure-MariaDB {
    & cmake --fresh `
        -S (Join-Path $Root "mariadb") `
        -B $BuildDir `
        -G Ninja `
        -C $Profile `
        @CMakeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "MariaDB embedded configure failed"
    }
}

function Build-MariaDB {
    & cmake --build $BuildDir --target mysqlserver
    if ($LASTEXITCODE -ne 0) {
        throw "MariaDB embedded build failed"
    }
    $Archive = Join-Path $BuildDir "libmysqld/mysqlserver.lib"
    if (-not (Test-Path $Archive)) {
        throw "MariaDB embedded archive was not produced: $Archive"
    }
    Write-Output "archive=$Archive"
    Write-Output "size_bytes=$((Get-Item $Archive).Length)"
}

switch ($Command) {
    "configure" { Configure-MariaDB }
    "build" { Build-MariaDB }
    "ensure" {
        if (-not (Test-Path (Join-Path $BuildDir "CMakeCache.txt"))) {
            Configure-MariaDB
        }
        Build-MariaDB
    }
    "all" {
        Configure-MariaDB
        Build-MariaDB
    }
}
