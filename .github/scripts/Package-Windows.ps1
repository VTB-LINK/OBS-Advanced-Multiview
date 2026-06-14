[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"
    $PortableOutputName = "${ProductName}-${ProductVersion}-windows-${Target}-portable"
    $InstallRoot = "${ProjectRoot}/release/${Configuration}/${ProductName}"
    $PortableRoot = "${ProjectRoot}/release/${PortableOutputName}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip",
            $PortableRoot
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}" -Exclude "${OutputName}*.*", "${PortableOutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs

    Log-Group "Archiving ${ProductName} portable layout..."
    $PortableBinPath = "${PortableRoot}/obs-plugins/64bit"
    $PortableDataPath = "${PortableRoot}/data/obs-plugins/${ProductName}"

    New-Item -Path $PortableBinPath -ItemType Directory -Force | Out-Null
    New-Item -Path $PortableDataPath -ItemType Directory -Force | Out-Null

    Copy-Item -Path "${InstallRoot}/bin/64bit/*" -Destination $PortableBinPath -Recurse -Force
    if ( Test-Path "${InstallRoot}/data" ) {
        Copy-Item -Path "${InstallRoot}/data/*" -Destination $PortableDataPath -Recurse -Force
    }

    $PortableCompressArgs = @{
        Path = (Get-ChildItem -Path $PortableRoot)
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${PortableOutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @PortableCompressArgs
    Remove-Item -Path $PortableRoot -Recurse -Force
    Log-Group
}

Package
