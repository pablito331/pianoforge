# Baixa o VST3 SDK para third_party/vst3sdk. Mesmo contrato do get_vst3sdk.sh:
# tarball pinado por tag, nada de git. Licenca nao e motivo de nao vendorizar: desde a
# serie 3.8 o SDK e MIT, entao o binario VST3 sai MIT junto (ver NOTICE).
param(
    [string]$Tag = $env:VST3SDK_TAG,
    [string]$Dest
)
$ErrorActionPreference = 'Stop'
if (-not $Tag) { $Tag = 'v3.8.1_build_84' }
$root = Split-Path -Parent $PSScriptRoot
if (-not $Dest) { $Dest = Join-Path $root 'third_party\vst3sdk' }

# Staging dentro do repo, nao em $env:TEMP: em ambiente encaixotado a escrita fora
# do workspace e negada e o download some sem erro visivel.
$tmp = Join-Path $root '_tmp\vst3sdk_stage'
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $tmp -Force | Out-Null
try {
    # Cada repo e o CONTEUDO de uma pasta do SDK, nao a pasta: vst3_pluginterfaces
    # traz base/gui/vst na raiz. test/ e samples/ ficam de fora (nao compilam aqui).
    $jobs = @(
        @{ repo = 'vst3_pluginterfaces'; dir = 'pluginterfaces'; items = @('base', 'gui', 'vst', 'LICENSE.txt') },
        @{ repo = 'vst3_base';           dir = 'base';           items = @('source', 'thread', 'LICENSE.txt') },
        @{ repo = 'vst3_public_sdk';     dir = 'public.sdk';     items = @('source', 'LICENSE.txt') }
    )
    New-Item -ItemType Directory -Path $Dest -Force | Out-Null
    foreach ($j in $jobs) {
        $url = "https://codeload.github.com/steinbergmedia/$($j.repo)/tar.gz/refs/tags/$Tag"
        $tgz = Join-Path $tmp "$($j.repo).tgz"
        Write-Output "vst3: $($j.repo) $Tag"
        Invoke-WebRequest -Uri $url -OutFile $tgz -UseBasicParsing -TimeoutSec 180
        $out = Join-Path $tmp $j.repo
        New-Item -ItemType Directory -Path $out -Force | Out-Null
        tar -xzf $tgz -C $out
        $src = Get-ChildItem $out -Directory | Select-Object -First 1
        $to = Join-Path $Dest $j.dir
        if (Test-Path $to) { Remove-Item $to -Recurse -Force }
        New-Item -ItemType Directory -Path $to -Force | Out-Null
        foreach ($item in $j.items) {
            $from = Join-Path $src.FullName $item
            if (-not (Test-Path $from)) { throw "$item nao encontrado em $($j.repo)" }
            Copy-Item $from $to -Recurse -Force
        }
    }
    Set-Content -Path (Join-Path $Dest '.tag') -Value $Tag -NoNewline
    Write-Output "vst3: ok em $Dest ($Tag)"
}
finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
