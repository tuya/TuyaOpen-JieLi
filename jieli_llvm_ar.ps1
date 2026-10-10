param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments
)

$environment = [Environment]::GetEnvironmentVariables()
if ($environment.Contains('JIELI_TOOL_DIR')) {
    $toolDir = [string]$environment['JIELI_TOOL_DIR']
    if ([string]::IsNullOrWhiteSpace($toolDir)) {
        Write-Error 'JIELI_TOOL_DIR is set but empty'
        exit 1
    }
} else {
    $portableToolDir = Join-Path $PSScriptRoot '.tools\portable-jieli-windows\pi32\bin'
    if (Test-Path -LiteralPath (Join-Path $portableToolDir 'llvm-ar.exe')) {
        $toolDir = $portableToolDir
    } elseif (Test-Path -LiteralPath 'C:\JL\pi32\bin\llvm-ar.exe') {
        $toolDir = 'C:\JL\pi32\bin'
    }
}
if (-not $toolDir) {
    Write-Error "Jieli llvm-ar not found; set JIELI_TOOL_DIR or prepare the Windows portable toolchain"
    exit 1
}

$rewritten = [System.Collections.Generic.List[string]]::new()
$temporaryFiles = [System.Collections.Generic.List[string]]::new()

try {
    foreach ($argument in $Arguments) {
        if ($argument.StartsWith('@')) {
            $source = (Resolve-Path -LiteralPath $argument.Substring(1)).Path
            $temporary = Join-Path ([System.IO.Path]::GetTempPath()) ("jieli-ar-" + [guid]::NewGuid() + '.rsp')
            $content = [System.IO.File]::ReadAllText($source)
            [System.IO.File]::WriteAllText(
                $temporary,
                $content.Replace('\', '/'),
                [System.Text.UTF8Encoding]::new($false)
            )
            $rewritten.Add('@' + $temporary)
            $temporaryFiles.Add($temporary)
        } else {
            $rewritten.Add($argument)
        }
    }

    $llvmAr = Join-Path $toolDir 'llvm-ar.exe'
    & $llvmAr @rewritten
    $status = $LASTEXITCODE
} catch {
    Write-Error $_
    $status = 1
} finally {
    foreach ($temporary in $temporaryFiles) {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

exit $status
