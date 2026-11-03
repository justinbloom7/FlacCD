param(
   [Parameter(Mandatory = $true)][string]$Directory,
   [Parameter(Mandatory = $true)][string]$Publisher,
   [Parameter(Mandatory = $true)][string]$Product,
   [Parameter(Mandatory = $true)][string]$Version
)
$ErrorActionPreference = 'Stop'
$descriptions = @{
   'flaccd.exe' = 'FlacCD audio CD mount utility'
   'flaccd-agent.exe' = 'FlacCD background mount agent'
   'flaccd-info.exe' = 'FlacCD FLAC metadata inspector'
   'flaccd-export.exe' = 'FlacCD CDDA export utility'
   'flaccd-bin-cue.exe' = 'FlacCD BIN/CUE export utility'
}
foreach ($name in $descriptions.Keys) {
   $info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $Directory $name))
   if ($info.CompanyName -cne $Publisher -or $info.ProductName -cne $Product -or
       $info.FileDescription -cne $descriptions[$name] -or $info.FileVersion -cne $Version -or
       $info.ProductVersion -cne $Version -or $info.OriginalFilename -cne $name) {
      throw "Incorrect VERSIONINFO for ${name}: $($info | Format-List | Out-String)"
   }
}
Write-Output 'All five executable VERSIONINFO resources passed.'
