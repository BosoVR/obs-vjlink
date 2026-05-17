param(
    [string]$Root = (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
)

$ErrorActionPreference = 'Stop'
$failures = New-Object System.Collections.Generic.List[string]

function Add-Failure {
    param([string]$Message)
    $failures.Add($Message)
}

function Get-EffectIdFromMeta {
    param($Json, [string]$Path)
    if ($Json.PSObject.Properties['effect_id']) { return [string]$Json.effect_id }
    if ($Json.PSObject.Properties['id']) { return [string]$Json.id }
    return [IO.Path]::GetFileNameWithoutExtension($Path)
}

$effectsRoot = Join-Path $Root 'effects'
$metaRoot = Join-Path $Root 'effects_meta'

if (-not (Test-Path -LiteralPath $effectsRoot)) {
    Add-Failure 'Missing effects directory.'
} elseif (-not (Test-Path -LiteralPath $metaRoot)) {
    Add-Failure 'Missing effects_meta directory.'
} else {
    $shaderFiles = Get-ChildItem -LiteralPath $effectsRoot -Recurse -Filter '*.effect' |
        Where-Object {
            $_.Name -notin @(
                'debug_solid.effect',
                'vjlink_test.effect',
                'debug_overlay.effect',
                'luma_alpha.effect',
                'videowall_blit.effect',
                'particle_render.effect',
                'particle_sim.effect'
            ) -and
            $_.FullName -notmatch '\\backup\\'
        } |
        Sort-Object FullName

    $metaFiles = Get-ChildItem -LiteralPath $metaRoot -Recurse -Filter '*.json' | Sort-Object FullName
    $metaById = @{}
    foreach ($metaFile in $metaFiles) {
        try {
            $json = Get-Content -LiteralPath $metaFile.FullName -Raw | ConvertFrom-Json
        } catch {
            Add-Failure "Invalid metadata JSON: $($metaFile.FullName.Substring($Root.TrimEnd('\').Length + 1))"
            continue
        }

        $id = Get-EffectIdFromMeta $json $metaFile.FullName
        if ($id) {
            $metaById[$id] = [pscustomobject]@{ Json = $json; Path = $metaFile.FullName }
        }
    }

    foreach ($shader in $shaderFiles) {
        $id = [IO.Path]::GetFileNameWithoutExtension($shader.Name)
        if (-not $metaById.ContainsKey($id)) {
            Add-Failure "Effect '$id' has a shader but no metadata JSON."
            continue
        }

        $meta = $metaById[$id].Json
        $params = @()
        if ($meta.PSObject.Properties['controls'] -and $meta.controls -is [array]) {
            $params = @($meta.controls)
        } elseif ($meta.PSObject.Properties['params'] -and $meta.params -is [array]) {
            $params = @($meta.params)
        }

        $visibleParams = @($params | Where-Object {
            $name = ''
            if ($_.PSObject.Properties['param']) { $name = [string]$_.param }
            elseif ($_.PSObject.Properties['id']) { $name = [string]$_.id }
            elseif ($_.PSObject.Properties['name']) { $name = [string]$_.name }
            $name -and $name -notmatch '^(input_tex|image|prev_tex|audio_tex|has_input_source|rms|chronotensity|band_activation|pad_trigger|pad_velocity|media_tex)$' -and
            $name -notmatch '^text_char_\d+$' -and
            $name -notmatch '^ring\d+_char_\d+$'
        })

        if ($visibleParams.Count -lt 3) {
            Add-Failure "Effect '$id' exposes fewer than 3 visible effect-specific settings."
        }
    }
}

$haloShader = Join-Path $Root 'effects\geometric\halo_text_logo_tunnel.effect'
$haloMeta = Join-Path $Root 'effects_meta\halo_text_logo_tunnel.json'

if (-not (Test-Path -LiteralPath $haloShader)) {
    Add-Failure 'Missing halo_text_logo_tunnel shader.'
} else {
    $shaderText = Get-Content -LiteralPath $haloShader -Raw
    if ($shaderText -notmatch 'choice == 6' -or $shaderText -notmatch 'return 2; if \(pos == 1\) return 15; if \(pos == 2\) return 19; if \(pos == 3\) return 15') {
        Add-Failure 'Halo shader does not define BOSO as preset word 6.'
    }
    foreach ($ring in 1..4) {
        foreach ($idx in 0..11) {
            $uniform = "ring${ring}_char_${idx}"
            if ($shaderText -notmatch "\b$uniform\b") {
                Add-Failure "Halo shader missing custom text uniform '$uniform'."
                break
            }
        }
    }
    if ($shaderText -notmatch 'customRingCharAt') {
        Add-Failure 'Halo shader does not read custom ring text through customRingCharAt.'
    }
}

if (-not (Test-Path -LiteralPath $haloMeta)) {
    Add-Failure 'Missing halo_text_logo_tunnel metadata.'
} else {
    $metaText = Get-Content -LiteralPath $haloMeta -Raw
    if ($metaText -notmatch '"BOSO"' -or $metaText -notmatch '"main_text".*"default": 6') {
        Add-Failure 'Halo metadata does not set BOSO as the default ring text.'
    }
    foreach ($ring in 1..4) {
        foreach ($idx in 0..11) {
            $uniform = "ring${ring}_char_${idx}"
            if ($metaText -notmatch """$uniform""") {
                Add-Failure "Halo metadata missing custom text param '$uniform'."
                break
            }
        }
    }
    foreach ($control in @('ring_text_mode', 'ring_text_active')) {
        if ($metaText -notmatch """$control""") {
            Add-Failure "Halo metadata missing '$control'."
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host 'Effect settings contract failed:' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host 'Effect settings contract passed.' -ForegroundColor Green
