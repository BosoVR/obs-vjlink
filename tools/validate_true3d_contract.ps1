param(
  [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'

function Require-Text {
  param(
    [string]$Path,
    [string]$Pattern,
    [string]$Label
  )

  $full = Join-Path $Root $Path
  if (!(Test-Path $full)) {
    throw "Missing file: $Path"
  }
  $text = Get-Content -Raw -Path $full
  if ($text -notmatch $Pattern) {
    throw "Missing contract: $Label in $Path"
  }
}

Require-Text 'src/rendering/compositor.h' 'struct vjlink_engine3d \*engine3d' 'compositor owns a 3D engine'
Require-Text 'src/rendering/compositor.c' 'vjlink_engine3d_create\(' '3D engine creation'
Require-Text 'src/rendering/compositor.c' 'vjlink_engine3d_destroy\(' '3D engine destruction'
Require-Text 'src/rendering/compositor.c' 'GS_Z24_S8' 'depth-enabled chain render targets'
Require-Text 'src/rendering/compositor.c' 'render_effect_node_3d_scene' '3D compositor branch'
Require-Text 'src/rendering/compositor.c' 'No-black safety' 'fullscreen fallback before mesh rendering'
Require-Text 'src/rendering/compositor.c' 'draw_fullscreen_effect_draw' 'shared fullscreen fallback draw'
Require-Text 'src/rendering/compositor.c' 'gs_perspective\(' 'perspective projection'
Require-Text 'src/rendering/compositor.c' 'gs_enable_depth_test\(true\)' 'depth test enabled'
Require-Text 'src/rendering/compositor.c' 'vjlink_mesh_draw\(' 'mesh draw call'
Require-Text 'src/rendering/effect_system.c' 'GS_SHADER_PARAM_MATRIX4X4' 'matrix shader parameter defaults'
Require-Text 'src/rendering/effect_system.c' 'gs_effect_set_matrix4' 'matrix shader parameter binding'
Require-Text 'effects/3d/logo_extrude_3d.effect' 'technique DrawMesh' 'logo mesh shader technique'
Require-Text 'effects/geometric/halo_text_logo_tunnel.effect' 'technique DrawMesh' 'halo mesh shader technique'
Require-Text 'effects/geometric/halo_text_logo_tunnel.effect' 'mesh_text_lane' 'mesh lane text uniform'
Require-Text 'web-ui/vjlink-control.html' 'setParamBandCurve' 'main param automation curve control'
Require-Text 'web-ui/vjlink-control.html' 'setParamBandSmoothing' 'main param automation smoothing control'
Require-Text 'web-ui/vjlink-control.html' 'applyAutomationCurve' 'audio automation shaping'

Write-Host 'True 3D contract OK'
