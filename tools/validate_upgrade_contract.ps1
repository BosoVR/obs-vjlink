param(
    [string]$Root = (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
)

$ErrorActionPreference = 'Stop'
$failures = New-Object System.Collections.Generic.List[string]

function Assert-Text {
    param(
        [string]$Path,
        [string]$Pattern,
        [string]$Message
    )
    $full = Join-Path $Root $Path
    if (-not (Test-Path -LiteralPath $full)) {
        $failures.Add("Missing file: $Path")
        return
    }
    $text = Get-Content -LiteralPath $full -Raw
    if ($text -notmatch $Pattern) {
        $failures.Add($Message)
    }
}

function Assert-File {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath (Join-Path $Root $Path))) {
        $failures.Add("Missing file: $Path")
    }
}

$metadataValidator = Join-Path $Root 'tools\validate_effect_meta_v2.ps1'
if (-not (Test-Path -LiteralPath $metadataValidator)) {
    $failures.Add('Missing file: tools\validate_effect_meta_v2.ps1')
} else {
    $metadataOutput = & powershell -NoProfile -ExecutionPolicy Bypass -File $metadataValidator -Root $Root 2>&1
    $metadataExitCode = $LASTEXITCODE
    if ($metadataExitCode -ne 0) {
        $message = ($metadataOutput | Out-String).Trim()
        $failures.Add("Effect metadata v2 validation failed: $message")
    }
}

$settingsValidator = Join-Path $Root 'tools\validate_effect_settings_contract.ps1'
if (-not (Test-Path -LiteralPath $settingsValidator)) {
    $failures.Add('Missing file: tools\validate_effect_settings_contract.ps1')
} else {
    $settingsOutput = & powershell -NoProfile -ExecutionPolicy Bypass -File $settingsValidator -Root $Root 2>&1
    $settingsExitCode = $LASTEXITCODE
    if ($settingsExitCode -ne 0) {
        $message = ($settingsOutput | Out-String).Trim()
        $failures.Add("Effect settings contract failed: $message")
    }
}

$true3dValidator = Join-Path $Root 'tools\validate_true3d_contract.ps1'
if (-not (Test-Path -LiteralPath $true3dValidator)) {
    $failures.Add('Missing file: tools\validate_true3d_contract.ps1')
} else {
    $true3dOutput = & powershell -NoProfile -ExecutionPolicy Bypass -File $true3dValidator -Root $Root 2>&1
    $true3dExitCode = $LASTEXITCODE
    if ($true3dExitCode -ne 0) {
        $message = ($true3dOutput | Out-String).Trim()
        $failures.Add("True 3D contract failed: $message")
    }
}

Assert-Text 'src\rendering\effect_system.h' 'p_macro_energy' 'Macro uniform handles are missing from effect_system.h.'
Assert-Text 'src\rendering\effect_system.c' 'macro_energy' 'Macro uniforms are not cached/bound in effect_system.c.'
Assert-Text 'src\rendering\effect_system.c' 'pad_trigger' 'Pad trigger uniforms are not cached/bound in effect_system.c.'
Assert-Text 'src\vjlink_context.h' 'pad_trigger' 'Pad runtime signal fields are missing from vjlink_context.h.'
Assert-Text 'src\controls\websocket_handler.c' 'SetPadState' 'SetPadState WebSocket request is not registered.'
Assert-Text 'src\ui\properties_builder.c' '\{"particles",' 'OBS properties still use the wrong particle category id.'
Assert-Text 'src\rendering\effect_system.c' 'debug_solid' 'Debug solid effect is not hidden from user effect lists.'
Assert-Text 'src\rendering\effect_system.c' 'vjlink_test' 'VJLink test effect is not hidden from user effect lists.'
Assert-File 'effects\flash\hardtechno_strobe.effect'
Assert-File 'effects_meta\flash\hardtechno_strobe.json'
Assert-Text 'web-ui\vjlink-control.html' 'hardtechno_strobe' 'Web UI does not expose the hardtechno strobe effect.'
Assert-Text 'web-ui\vjlink-control.html' 'basic-mode' 'Web UI does not apply a Basic-specific mode class.'
Assert-Text 'web-ui\vjlink-control.html' 'activeEffect && activeBandIndex < 0' 'Profile capture can still overwrite main effect params from a band tab.'
Assert-Text 'web-ui\vjlink-control.html' 'decay_speed' 'Kick Flash UI still uses stale decay parameter name.'
Assert-Text 'web-ui\vjlink-control.html' 'sharpness' 'Snare Flash UI still uses stale decay parameter name.'
Assert-Text 'web-ui\vjlink-control.html' 'wave_speed' 'Radial Flash UI still uses stale speed parameter name.'
Assert-Text 'effects\flash\strobe_hard.effect' 'kick_onset' 'Hard strobe is not onset-aware.'
Assert-Text 'effects\flash\strobe_color.effect' 'strobe_safety' 'Color strobe does not respect strobe safety.'
Assert-Text 'effects\flash\flash_kick.effect' 'max\(band_activation, max\(kick_onset' 'Kick flash does not use kick onset as a trigger.'
Assert-Text 'effects\flash\flash_snare.effect' 'high_mid_hit = max\(snare_onset' 'Snare flash does not use snare onset/high-mid as a trigger.'
Assert-Text 'effects\flash\flash_snare.effect' 'trigger_threshold' 'Snare flash is missing trigger threshold control.'
Assert-Text 'effects\flash\flash_radial.effect' 'beat_1_4' 'Radial flash is not beat-reset.'
Assert-Text 'effects\flash\flash_shake.effect' 'macro_chaos' 'Flash shake does not respond to chaos macro.'
Assert-Text 'effects\geometric\type_animator.effect' 'beat_bounce' 'Type Animator is missing beat-bounce toggle.'
Assert-Text 'effects_meta\type_animator.json' 'text_char_11' 'Type Animator metadata does not expose hidden custom text uniforms to the renderer.'
Assert-Text 'effects\geometric\screen_ring.effect' 'customRingCharAt' 'Screen Ring is missing custom ring text decoding.'
Assert-Text 'effects\geometric\screen_ring.effect' 'choice == 0' 'Screen Ring is missing BOSO as text preset 0.'
Assert-Text 'effects_meta\screen_ring.json' 'ring4_char_11' 'Screen Ring metadata does not expose hidden custom ring text uniforms.'
Assert-Text 'effects_meta\screen_ring.json' '"BOSO"' 'Screen Ring metadata does not expose BOSO as default text.'
Assert-Text 'web-ui\vjlink-control.html' 'renderHaloRingTextPanel' 'Web UI is missing Screen Ring custom text controls.'
Assert-Text 'effects\3d\sphere_field.effect' 'rotation_axis' 'Sphere Field is missing rotation axis control.'
Assert-Text 'effects\retro\vaporwave_sunset.effect' 'palmSilhouette' 'Vaporwave Sunset is missing upgraded silhouette visuals.'
Assert-Text 'effects\postprocess\bloom_pro.effect' 'standalone preview source' 'Bloom Pro still has no standalone fallback.'
Assert-Text 'web-ui\vjlink-control.html' 'trigger_threshold' 'Web UI is missing Snare Flash trigger threshold control.'
Assert-Text 'web-ui\vjlink-control.html' 'beat_bounce' 'Web UI is missing Type Animator beat-bounce control.'

if ($failures.Count -gt 0) {
    Write-Host "VJLink upgrade contract failed:" -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host "VJLink upgrade contract passed." -ForegroundColor Green
