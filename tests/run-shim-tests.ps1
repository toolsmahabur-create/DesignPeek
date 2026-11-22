# End-to-end test: every sample goes through the real native shim -> worker process -> bitmap.
param(
    [Parameter(Mandatory)] [string] $ShimTest,
    [Parameter(Mandatory)] [string] $Shim,
    [Parameter(Mandatory)] [string] $Samples
)

$ErrorActionPreference = 'Stop'

$unicodeFile = Get-ChildItem -LiteralPath (Join-Path $Samples 'unicode') -Recurse -Filter '*.psd' | Select-Object -First 1

$cases = @(
    @{ Path = 'psd_with_thumb.psd'; Size = 256; Expect = 'ok' }
    @{ Path = 'psd_with_thumb.psd'; Size = 32;  Expect = 'ok' }
    @{ Path = 'psb_with_thumb.psb'; Size = 256; Expect = 'ok' }
    @{ Path = 'psd_bgr_1033.psd';   Size = 256; Expect = 'ok' }
    @{ Path = 'large.psb';          Size = 256; Expect = 'ok' }
    @{ Path = 'psd_no_thumb.psd';   Size = 256; Expect = 'ok' }
    @{ Path = 'psd_rle.psd';        Size = 256; Expect = 'ok' }
    @{ Path = 'psd_cmyk.psd';       Size = 256; Expect = 'ok' }
    @{ Path = 'psd_layers.psd';     Size = 96;  Expect = 'ok' }
    @{ Path = 'zero.psd';           Size = 256; Expect = 'fail' }
    @{ Path = 'corrupt.psd';        Size = 256; Expect = 'fail' }
    @{ Path = 'truncated.psd';      Size = 256; Expect = 'fail' }
    @{ Path = $unicodeFile.FullName; Size = 256; Expect = 'ok' }
)

$failed = 0
foreach ($case in $cases) {
    $file = if ([IO.Path]::IsPathRooted($case.Path)) { $case.Path } else { Join-Path $Samples $case.Path }
    Write-Host "$([IO.Path]::GetFileName($file)) @ $($case.Size)px (expect $($case.Expect))"
    & $ShimTest $Shim $file $case.Size $case.Expect
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  -> FAIL' -ForegroundColor Red
        $failed++
    } else {
        Write-Host '  -> PASS' -ForegroundColor Green
    }
}

if ($failed -gt 0) {
    Write-Error "$failed shim test(s) failed"
}
Write-Host 'All shim tests passed.'
