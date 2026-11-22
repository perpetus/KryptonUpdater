[CmdletBinding()]
param(
  [Parameter(Mandatory)][ValidateSet('create-key', 'sign', 'verify')][string]$Command,
  [switch]$ReadKeyFromStdin,
  [string]$KeyPath = (Join-Path $env:USERPROFILE '.krypton/signing/release-key.dpapi'),
  [string]$InputFile,
  [string]$SignatureFile,
  [string]$TrustFile = (Join-Path $PSScriptRoot '../resources/update-trust.json')
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Add-Type -AssemblyName System.Security.Cryptography
# Keep Span-based key import and secure clearing inside managed code so the
# PowerShell binder only handles byte arrays, including on older hosts.
Add-Type -TypeDefinition @'
using System.Security.Cryptography;

namespace Krypton.ReleaseSigning {
  public static class KeyMaterial {
    public static void Import(ECDsa key, byte[] bytes) {
      key.ImportPkcs8PrivateKey(bytes, out int consumed);
      if (consumed != bytes.Length) {
        throw new CryptographicException("Unexpected trailing private-key data.");
      }
    }

    public static void Clear(byte[] bytes) {
      CryptographicOperations.ZeroMemory(bytes);
    }
  }
}
'@
$ecdsa = [Security.Cryptography.ECDsa]::Create([Security.Cryptography.ECCurve+NamedCurves]::nistP256)
$signingStream = $null
try {
  if ($Command -eq 'create-key') {
    if (Test-Path -LiteralPath $KeyPath) { throw 'A release key already exists. Refusing to replace it.' }
    $directory = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($KeyPath))
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $acl = [Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true, $false)
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($identity, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-5-18'), 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
    Set-Acl -LiteralPath $directory -AclObject $acl
    $private = $ecdsa.ExportPkcs8PrivateKey()
    try {
      $encrypted = [Security.Cryptography.ProtectedData]::Protect($private, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
      [IO.File]::WriteAllBytes([IO.Path]::GetFullPath($KeyPath), $encrypted)
    } finally { [Krypton.ReleaseSigning.KeyMaterial]::Clear($private) }
    $public = $ecdsa.ExportParameters($false)
    $point = [byte[]](@(4) + $public.Q.X + $public.Q.Y)
    $keyId = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($point)).ToLowerInvariant()
    $trust = [ordered]@{ algorithm='ecdsa-p256-sha256-p1363'; key_id=$keyId; repository='perpetus/krypton-windows'; public_key=[Convert]::ToHexString($point).ToLowerInvariant() }
    [IO.File]::WriteAllText([IO.Path]::GetFullPath($TrustFile), ($trust | ConvertTo-Json) + "`n", [Text.UTF8Encoding]::new($false))
    $bytes = ($point | ForEach-Object { '0x{0:x2}' -f $_ }) -join ', '
    $header = "#pragma once`n#include <array>`n#include <cstdint>`nnamespace kr::updater::windows {`ninline constexpr std::array<std::uint8_t, 65> kReleasePublicKey = {$bytes};`ninline constexpr char kReleaseRepository[] = `"perpetus/krypton-windows`";`n} // namespace kr::updater::windows`n"
    [IO.File]::WriteAllText((Join-Path $root 'src/windows/include/krypton/updater/windows/release_trust.hpp'), $header, [Text.UTF8Encoding]::new($false))
    Write-Output "Release public key: $keyId"
    Write-Output "Private key protected for this Windows account: $KeyPath"
    return
  }
  if (-not $InputFile) { throw 'InputFile is required.' }
  if (-not $SignatureFile) { $SignatureFile = "$InputFile.sig" }
  $signingStream = [IO.File]::OpenRead((Resolve-Path -LiteralPath $InputFile).Path)
  if ($Command -eq 'sign') {
    # The hosted signer receives the key through its private input pipe.
    # Local signing still reads the account-bound DPAPI key.
    [byte[]]$private = if ($ReadKeyFromStdin) {
      if ($PSBoundParameters.ContainsKey('KeyPath')) { throw 'Select one key source.' }
      $encoded = [Console]::In.ReadLine()
      if (-not $encoded -or $encoded.Length -gt 1024) { throw 'Invalid hosted signing key input.' }
      try { [Convert]::FromBase64String($encoded) }
      finally { $encoded = $null }
    } else {
      [Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $KeyPath).Path), $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
    }
    try {
      [Krypton.ReleaseSigning.KeyMaterial]::Import($ecdsa, $private)
      $trust = Get-Content -LiteralPath $TrustFile -Raw | ConvertFrom-Json
      $public = $ecdsa.ExportParameters($false)
      $point = [byte[]](@(4) + $public.Q.X + $public.Q.Y)
      if ([Convert]::ToHexString($point).ToLowerInvariant() -cne $trust.public_key) { throw 'The signing key does not match the pinned public key.' }
      $signature = $ecdsa.SignData($signingStream, [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
      [IO.File]::WriteAllBytes([IO.Path]::GetFullPath($SignatureFile), $signature)
    } finally { [Krypton.ReleaseSigning.KeyMaterial]::Clear($private) }
  } else {
    $trust = Get-Content -LiteralPath $TrustFile -Raw | ConvertFrom-Json
    $point = [Convert]::FromHexString($trust.public_key)
    $parameters = [Security.Cryptography.ECParameters]::new()
    $parameters.Curve = [Security.Cryptography.ECCurve+NamedCurves]::nistP256
    $publicPoint = [Security.Cryptography.ECPoint]::new()
    $publicPoint.X = $point[1..32]
    $publicPoint.Y = $point[33..64]
    $parameters.Q = $publicPoint
    $ecdsa.ImportParameters($parameters)
    $signature = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $SignatureFile).Path)
    if (-not $ecdsa.VerifyData($signingStream, $signature, [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)) { throw 'Release signature verification failed.' }
    Write-Output "Verified: $InputFile"
  }
} finally { if ($signingStream) { $signingStream.Dispose() }; $ecdsa.Dispose() }
