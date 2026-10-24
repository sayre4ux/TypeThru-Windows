# Signs an update file with the TypeThru-Update CNG key, writing <file>.sig (base64).
# Usage: powershell -File scripts\sign-update.ps1 path\to\TypeThru-0.1.0-win64.zip

param([Parameter(Mandatory = $true)][string]$File)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $File)) { throw "No such file: $File" }

$key = [System.Security.Cryptography.CngKey]::Open('TypeThru-Update')
$ecdsa = [System.Security.Cryptography.ECDsaCng]::new($key)
$data = [System.IO.File]::ReadAllBytes((Resolve-Path $File))
$signature = $ecdsa.SignData($data, [System.Security.Cryptography.HashAlgorithmName]::SHA256)
[System.IO.File]::WriteAllText("$File.sig", [System.Convert]::ToBase64String($signature))
Write-Host "Signed: $File.sig ($($signature.Length) byte raw signature, base64-encoded)"
