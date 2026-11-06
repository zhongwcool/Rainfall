param(
    [Parameter(Mandatory = $true)]
    [string]$OutFile
)

$ErrorActionPreference = 'Stop'
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

function Get-RawVersion {
    if ($env:RAINFALL_VERSION_TAG -and $env:RAINFALL_VERSION_TAG.Trim()) {
        return $env:RAINFALL_VERSION_TAG.Trim()
    }

    $repoRoot = Split-Path -Parent $PSScriptRoot
    Push-Location $repoRoot
    try {
        $exact = & git describe --tags --exact-match 2>$null
        if ($LASTEXITCODE -eq 0 -and $exact) {
            return "$exact".Trim()
        }

        $described = & git describe --tags --always 2>$null
        if ($LASTEXITCODE -eq 0 -and $described) {
            return "$described".Trim()
        }
    }
    finally {
        Pop-Location
    }

    return '0.0.0'
}

$label = (Get-RawVersion) -replace '^[vV]', ''
if (-not $label) {
    $label = '0.0.0'
}

$major = 0
$minor = 0
$patch = 0
$build = 0
if ($label -match '^(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:\.(\d+))?') {
    $major = [int]$Matches[1]
    if ($Matches[2]) { $minor = [int]$Matches[2] }
    if ($Matches[3]) { $patch = [int]$Matches[3] }
    if ($Matches[4]) { $build = [int]$Matches[4] }
}

$escaped = ($label -replace '\\', '\\' -replace '"', '\"')
$content = @(
    '#pragma once'
    ''
    '// 由编译前事件根据 git tag 生成，请勿手改。'
    "#define RAINFALL_VERSION_MAJOR $major"
    "#define RAINFALL_VERSION_MINOR $minor"
    "#define RAINFALL_VERSION_PATCH $patch"
    "#define RAINFALL_VERSION_BUILD $build"
    "#define RAINFALL_VERSION_STRING `"$escaped`""
    "#define RAINFALL_VERSION_W L`"$escaped`""
    ''
) -join "`n"

$fullPath = [System.IO.Path]::GetFullPath($OutFile)
$existing = ''
if (Test-Path -LiteralPath $fullPath) {
    $existing = [System.IO.File]::ReadAllText($fullPath) -replace "`r`n", "`n"
}
if ($existing -ne $content) {
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($fullPath, $content, $utf8)
}

Write-Host "Rainfall version: $label"
