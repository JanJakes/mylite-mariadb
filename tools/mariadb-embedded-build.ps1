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
$WolfSSLDir = Join-Path $Root "mariadb/extra/wolfssl/wolfssl"
$WolfSSLCommit = "59f4fa568615396fbf381b073b220d1e8d61e4c2"

function Ensure-WolfSSL {
    $WolfSSLMarker = Join-Path $WolfSSLDir "wolfssl/src/crl.c"
    if (Test-Path $WolfSSLMarker) {
        $ActualCommit = (& git -C $WolfSSLDir rev-parse HEAD).Trim()
        if (($LASTEXITCODE -ne 0) -or ($ActualCommit -ne $WolfSSLCommit)) {
            throw "WolfSSL source must be the MariaDB 11.8.6 commit $WolfSSLCommit; found $ActualCommit"
        }
        return
    }

    if (Test-Path $WolfSSLDir) {
        $ExistingEntry = Get-ChildItem -Force $WolfSSLDir | Select-Object -First 1
        if ($ExistingEntry) {
            throw "Incomplete WolfSSL source exists at $WolfSSLDir; remove that dependency directory and retry"
        }
    } else {
        New-Item -ItemType Directory -Path $WolfSSLDir | Out-Null
    }

    & git -C $WolfSSLDir init
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to initialize the WolfSSL dependency directory"
    }
    & git -C $WolfSSLDir remote add origin https://github.com/wolfSSL/wolfssl.git
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to configure the WolfSSL dependency remote"
    }
    & git -C $WolfSSLDir fetch --depth=1 origin $WolfSSLCommit
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to fetch WolfSSL commit $WolfSSLCommit"
    }
    & git -C $WolfSSLDir checkout --detach FETCH_HEAD
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to check out WolfSSL commit $WolfSSLCommit"
    }
}

function Configure-MariaDB {
    Ensure-WolfSSL
    $Bison = Get-Command bison.exe -ErrorAction SilentlyContinue
    if (-not $Bison) {
        $Bison = Get-Command win_bison.exe -ErrorAction SilentlyContinue
    }
    if (-not $Bison) {
        throw "MariaDB embedded configure requires bison.exe or win_bison.exe"
    }
    & cmake --fresh `
        -S (Join-Path $Root "mariadb") `
        -B $BuildDir `
        -G Ninja `
        -C $Profile `
        "-DBISON_EXECUTABLE=$($Bison.Source)" `
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

    $Dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if (-not $Dumpbin) {
        throw "MariaDB embedded archive verification requires dumpbin.exe"
    }
    $ArchiveSymbols = (& $Dumpbin.Source /LINKERMEMBER:1 $Archive) -join "`n"
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to inspect the MariaDB embedded archive: $Archive"
    }
    foreach ($RequiredSymbol in @("mysql_server_init", "my_crc32c")) {
        if (-not $ArchiveSymbols.Contains($RequiredSymbol)) {
            throw "MariaDB embedded archive is incomplete: missing $RequiredSymbol"
        }
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
