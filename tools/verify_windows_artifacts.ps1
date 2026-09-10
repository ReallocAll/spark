[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Dll,

    [Parameter(Mandatory = $true)]
    [string]$Pdb,

    [Parameter(Mandatory = $false)]
    [string]$Version,

    [Parameter(Mandatory = $false)]
    [string]$ManifestPath,

    [Parameter(Mandatory = $false)]
    [string]$LlvmReadObj = "llvm-readobj",

    [Parameter(Mandatory = $false)]
    [string]$LlvmPdbUtil = "llvm-pdbutil",

    [Alias("GenerateManifest")]
    [switch]$WriteManifest,

    [switch]$VerifyHashesOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedNames = @("endstone_spark.dll", "endstone_spark.pdb")

function Fail([string]$Message) {
    throw "Windows artifact verification failed: $Message"
}

function Resolve-File([string]$Path, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        Fail "$Label path is empty"
    }
    try {
        $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    } catch {
        Fail "$Label is missing or unreadable: $Path"
    }
    if (-not (Test-Path -LiteralPath $resolved.Path -PathType Leaf)) {
        Fail "$Label is not a regular file: $Path"
    }
    return $resolved.Path
}

function Get-Basename([string]$Path, [string]$Label) {
    $name = [IO.Path]::GetFileName($Path)
    if ([string]::IsNullOrWhiteSpace($name) -or $name -ne $Path.Replace('/', '\').Split('\')[-1]) {
        Fail "$Label must name a regular file"
    }
    return $name
}

function Invoke-Llvm([string]$Tool, [string[]]$Arguments) {
    try {
        $output = (& $Tool @Arguments 2>&1 | Out-String)
        $exitCode = $LASTEXITCODE
    } catch {
        Fail "unable to invoke ${Tool}: $($_.Exception.Message)"
    }
    if ($exitCode -ne 0) {
        Fail "$Tool exited with code $exitCode"
    }
    if ([string]::IsNullOrWhiteSpace($output)) {
        Fail "$Tool returned no output"
    }
    return $output
}

function Normalize-Guid([string]$Value) {
    $normalized = $Value.Trim().Trim('{}').ToUpperInvariant()
    if ($normalized -notmatch '^[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}$') {
        Fail "invalid CodeView GUID: $Value"
    }
    return $normalized
}

function Get-CoffIdentity([string]$Output) {
    $lines = @($Output -split "`r?`n")
    $records = @()
    for ($index = 0; $index -lt $lines.Count; $index++) {
        if ($lines[$index].Trim() -notin @("DebugDirectory {", "DebugEntry {")) {
            continue
        }
        $block = [Collections.Generic.List[string]]::new()
        $closed = $false
        for ($index++; $index -lt $lines.Count; $index++) {
            if ($lines[$index].Trim() -eq "}") {
                $closed = $true
                break
            }
            $block.Add($lines[$index])
        }
        if (-not $closed) {
            Fail "PE debug directory output is malformed"
        }
        $blockText = $block -join "`n"
        if ($blockText -notmatch '(?im)^\s*Type\s*:\s*CodeView(?:\s|$)') {
            continue
        }
        $guidMatches = [regex]::Matches($blockText, '(?im)^\s*(?:PDBGUID|GUID)\s*:\s*(\{?[0-9A-Fa-f-]+\}?)\s*$')
        $ageMatches = [regex]::Matches($blockText, '(?im)^\s*(?:PDBAge|Age)\s*:\s*(\d+)\s*$')
        if ($guidMatches.Count -ne 1 -or $ageMatches.Count -ne 1) {
            Fail "PE CodeView identity is missing or ambiguous"
        }
        $records += [PSCustomObject]@{
            Guid = Normalize-Guid $guidMatches[0].Groups[1].Value
            Age = [UInt64]$ageMatches[0].Groups[1].Value
        }
    }
    if ($records.Count -ne 1) {
        Fail "PE CodeView identity is missing or ambiguous"
    }
    return $records[0]
}

function Get-PdbIdentity([string]$Output) {
    $guidMatches = [regex]::Matches($Output, '(?im)^\s*GUID\s*:\s*(\{?[0-9A-Fa-f-]+\}?)\s*$')
    if ($guidMatches.Count -eq 0) {
        $guidMatches = [regex]::Matches($Output, '(?im)^\s*Signature\s*:\s*(\{?[0-9A-Fa-f-]+\}?)\s*$')
    }
    $ageMatches = [regex]::Matches($Output, '(?im)^\s*Age\s*:\s*(\d+)\s*$')
    if ($guidMatches.Count -ne 1 -or $ageMatches.Count -ne 1) {
        Fail "PDB identity is missing or ambiguous"
    }
    return [PSCustomObject]@{
        Guid = Normalize-Guid $guidMatches[0].Groups[1].Value
        Age = [UInt64]$ageMatches[0].Groups[1].Value
    }
}

function Assert-Version([string]$Path, [string]$RequestedVersion) {
    if ($RequestedVersion -notmatch '^\d+\.\d+\.\d+$') {
        Fail "requested version must be X.Y.Z"
    }
    $versionParts = $RequestedVersion.Split('.') | ForEach-Object {
        try {
            [UInt32]::Parse($_)
        } catch {
            Fail "requested version components must be unsigned integers"
        }
    }
    $expected = "$RequestedVersion.0"
    try {
        $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
    } catch {
        Fail "unable to read PE version resources: $($_.Exception.Message)"
    }
    if ($info.FileVersion.Trim() -ne $expected -or $info.ProductVersion.Trim() -ne $expected) {
        Fail "PE FileVersion/ProductVersion do not equal $expected"
    }
    $versionFields = @(
        @{ Label = "FileVersion"; Values = @($info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart) }
        @{ Label = "ProductVersion"; Values = @($info.ProductMajorPart, $info.ProductMinorPart, $info.ProductBuildPart, $info.ProductPrivatePart) }
    )
    $expectedFields = @($versionParts[0], $versionParts[1], $versionParts[2], [UInt32]0)
    foreach ($field in $versionFields) {
        for ($index = 0; $index -lt $expectedFields.Count; $index++) {
            if ([UInt32]$field.Values[$index] -ne $expectedFields[$index]) {
                Fail "PE $($field.Label) numeric fields do not equal $expected"
            }
        }
    }
}

function Get-ManifestEntries([string]$Path) {
    $resolvedManifest = Resolve-File $Path "manifest"
    try {
        $lines = @(Get-Content -LiteralPath $resolvedManifest -Encoding UTF8 -ErrorAction Stop)
    } catch {
        Fail "manifest is unreadable: $Path"
    }
    if ($lines.Count -ne $ExpectedNames.Count) {
        Fail "manifest must contain exactly $($ExpectedNames.Count) entries"
    }
    $entries = @{}
    foreach ($line in $lines) {
        if ($line -notmatch '^([0-9A-Fa-f]{64}) {2}([^\s]+)$') {
            Fail "manifest contains a malformed entry"
        }
        $hash = $Matches[1].ToLowerInvariant()
        $name = $Matches[2]
        if ($name -ne [IO.Path]::GetFileName($name) -or $name.Contains('/') -or $name.Contains('\')) {
            Fail "manifest contains an unsafe path: $name"
        }
        if ($ExpectedNames -notcontains $name) {
            Fail "manifest contains a non-whitelisted file: $name"
        }
        if ($entries.ContainsKey($name)) {
            Fail "manifest contains a duplicate entry: $name"
        }
        $entries[$name] = $hash
    }
    foreach ($name in $ExpectedNames) {
        if (-not $entries.ContainsKey($name)) {
            Fail "manifest is missing $name"
        }
    }
    return $entries
}

function Get-Hashes([string]$DllPath, [string]$PdbPath) {
    return @{
        "endstone_spark.dll" = (Get-FileHash -LiteralPath $DllPath -Algorithm SHA256).Hash.ToLowerInvariant()
        "endstone_spark.pdb" = (Get-FileHash -LiteralPath $PdbPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Assert-Manifest([string]$Path, [string]$DllPath, [string]$PdbPath) {
    $entries = Get-ManifestEntries $Path
    $actual = Get-Hashes $DllPath $PdbPath
    foreach ($name in $ExpectedNames) {
        if ($entries[$name] -ne $actual[$name]) {
            Fail "manifest hash mismatch for $name"
        }
    }
}

$dllPath = Resolve-File $Dll "DLL"
$pdbPath = Resolve-File $Pdb "PDB"
if ((Get-Basename $dllPath "DLL") -ne "endstone_spark.dll" -or
    (Get-Basename $pdbPath "PDB") -ne "endstone_spark.pdb") {
    Fail "artifacts must be named endstone_spark.dll and endstone_spark.pdb"
}

if (-not $VerifyHashesOnly) {
    if ([string]::IsNullOrWhiteSpace($Version)) {
        Fail "-Version is required for full verification"
    }
    Assert-Version $dllPath $Version
    $coffIdentity = Get-CoffIdentity (Invoke-Llvm $LlvmReadObj @("--coff-debug-directory", $dllPath))
    $pdbIdentity = Get-PdbIdentity (Invoke-Llvm $LlvmPdbUtil @("dump", "-summary", $pdbPath))
    if ($coffIdentity.Guid -ne $pdbIdentity.Guid -or $coffIdentity.Age -ne $pdbIdentity.Age) {
        Fail "PE and PDB CodeView GUID+Age do not match"
    }
}

if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $ManifestPath = Join-Path (Split-Path -Parent $dllPath) "SHA256SUMS"
}
if ($WriteManifest) {
    $hashes = Get-Hashes $dllPath $pdbPath
    $manifestDirectory = Split-Path -Parent ([IO.Path]::GetFullPath($ManifestPath))
    if (-not (Test-Path -LiteralPath $manifestDirectory -PathType Container)) {
        Fail "manifest directory is missing: $manifestDirectory"
    }
    $content = @(
        "$($hashes['endstone_spark.dll'])  endstone_spark.dll"
        "$($hashes['endstone_spark.pdb'])  endstone_spark.pdb"
    ) -join "`n"
    try {
        [IO.File]::WriteAllText([IO.Path]::GetFullPath($ManifestPath), "$content`n", [Text.UTF8Encoding]::new($false))
    } catch {
        Fail "unable to write manifest: $($_.Exception.Message)"
    }
} else {
    Assert-Manifest $ManifestPath $dllPath $pdbPath
}

Write-Output "Verified endstone_spark.dll, endstone_spark.pdb, and SHA256SUMS"
