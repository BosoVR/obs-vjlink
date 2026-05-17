# VJLink UI, Settings, Saving, And Effect Overhaul Design

**Date:** 2026-05-16

**Goal:** Rebuild VJLink around a consistent effect-settings standard, reliable saved state, a simpler but adjustable Basic mode, a deep Pro VJ workflow, and two new flagship effects: 2D-logo-to-3D extrusion and the monochrome rotating text-ring logo tunnel from the provided GIF.

**Non-Negotiables**

- Basic mode must stay simple, but it must still allow real adjustment.
- Pro mode must expose the full VJ toolset.
- Every effect must have a consistent settings standard.
- Saved settings must reload consistently across OBS restarts, Web UI reloads, and plugin rebuilds.
- AI/Auto-VJ remains opt-in only.
- Every implementation wave must build, deploy to `F:\obs-studio`, and verify.

---

## Research Notes

### AudioLink Direction

VRChat AudioLink is useful as a mental model because it treats audio analysis as a standardized data stream that shaders can consume. VJLink should mirror that idea in OBS: bands, beat, onset, snare, and macros become first-class modulation sources instead of ad hoc per-effect hacks.

References:

- https://github.com/llealloo/audiolink
- https://vrctxl.github.io/Docs/docs/video-txl/third-party/audiolink
- https://ltcgi.dev/Advanced/Audiolink

### VJ Tool Direction

Resolume and VDMX point toward a clear live-performance structure:

- clip/effect triggering
- layer and effect stacks
- parameter inspection
- FFT/audio reactivity
- MIDI/OSC control
- saved presets and layouts

References:

- https://resolume.com/software/avenue-arena
- https://www.resolume.com/support/en/layouts
- https://vdmx.vidvox.net/tutorials/general-overview-of-vdmx

### GIF Analysis

User-provided file:

- `E:\Desktop\a_5305bba661600f3c178e033629481fbb.gif`
- 158 frames
- 600x240
- around 4.79 seconds

Observed visual components:

- harsh monochrome palette
- central dark logo or silhouette area
- multiple perspective text rings
- repeated words such as `HALO WHO?`
- text rings drift, rotate, and skew independently
- vertical barcode-like scan columns
- horizontal analog tearing
- smoke/noise haze
- bright white flash islands
- broken frame wipes and digital tearing

The implementation should not be a video playback clone. It should be a controllable procedural effect with the same visual grammar.

---

## Current System Problems

### Metadata Inconsistency

Current metadata mixes multiple schemas:

- effect identity can be `id` or `effect_id`
- parameter uniform can be `id` or `name`
- display text can be `name` or `label`
- Basic/Pro grouping is not standardized
- parameter persistence depends on Web UI assumptions

The current parser handles both schema styles in `src/rendering/effect_system.c`, but that compatibility layer should become a migration bridge, not the long-term source of truth.

### Saving Is Split

Current Web UI profiles are mainly browser `localStorage` based. This makes saved setups fragile:

- another browser profile can miss them
- OBS restarts do not guarantee plugin state is restored
- Web UI reload and backend state can drift
- old data can appear if the wrong OBS plugin path is loaded

### UI Is Too Dense In The Wrong Places

Basic and Pro are not separated enough by workflow. Basic still sees too much technical surface, while Pro does not yet have a complete inspector/settings structure.

---

## Effect Metadata Standard V2

Every effect gets a normalized `effect_meta_v2` JSON structure.

```json
{
  "schema_version": 2,
  "effect_id": "example_effect",
  "name": "Example Effect",
  "category": "glitch",
  "role": "filter",
  "performance_group": "post_glitch",
  "tags": ["hardtechno", "audio-reactive", "monochrome"],
  "quality_cost": "medium",
  "requires_input": true,
  "description": "Short human-readable description.",
  "defaults": {
    "basic_preset": "balanced",
    "quality": "balanced"
  },
  "controls": [
    {
      "param": "intensity",
      "label": "Intensity",
      "type": "float",
      "default": 0.6,
      "min": 0.0,
      "max": 1.0,
      "step": 0.01,
      "basic": true,
      "group": "Main",
      "audio_target": true,
      "save": true
    }
  ],
  "audio_link": {
    "recommended_band": "beat",
    "targets": ["intensity", "speed", "scale"]
  }
}
```

### Standard Control Groups

Every effect uses these groups where applicable:

- `Main`: enabled, mix, intensity, speed
- `Transform`: position, scale, rotation, perspective
- `AudioLink`: audio drive, source band, threshold, attack, release, beat bounce
- `Color`: palette, color mode, color A, color B, mono/invert
- `Texture/Input`: logo slot, input mix, alpha threshold
- `Glitch`: tear, jitter, scanlines, noise, RGB split
- `Performance`: quality, ray steps, sample count, particle count

### Basic Control Contract

Every user-facing effect should expose 4-8 Basic controls:

- `Enabled`
- `Intensity`
- `Speed`
- `Audio React`
- `Beat Bounce`
- `Scale` or `Size`
- `Color` or `Palette`
- one effect-specific hero control

Basic controls must never be fake. They must change actual shader/backend values.

### Pro Control Contract

Pro exposes every stable parameter grouped by purpose. Pro also exposes audio modulation per parameter:

- source: Off, Bass, LowMid, HighMid, Treble, Beat, Snare, Onset, Macro
- amount
- min
- max
- threshold
- attack
- release
- invert

---

## State And Saving V2

### State File

Add a backend-owned state file:

- `data/obs-plugins/obs-vjlink/state/vjlink_state_v2.json`

It stores:

- active UI mode
- active profile name
- active effect chain
- all effect parameters
- Basic control values
- Pro control values
- AudioLink mappings
- band effect slots
- pad banks
- logo paths and logo slots
- macro values
- strobe safety
- quality profile
- settings menu values
- AI mode and AI training data location, with AI mode defaulting to `off`

### Save Modes

- `Autosave`: debounced after user changes.
- `Save Profile`: named performance profile.
- `Save Show`: everything needed for a complete session.
- `Export Bundle`: portable `.vjlpack`.

### Load Order

On OBS/plugin startup:

1. Load effect registry and metadata.
2. Load state file.
3. Validate all saved effect IDs and params against metadata.
4. Apply backend state to compositor.
5. Start HTTP/Web UI.
6. Web UI requests canonical state from backend.
7. Web UI renders from backend state instead of guessing from localStorage.

### Migration

Old localStorage profiles remain importable. They are converted into `vjlink_state_v2` profile entries.

---

## Web UI Overhaul

### Basic: Club Deck

Basic screen sections:

- header: connection, loaded path, BPM, beat, Basic/Pro, settings
- AudioLink band strip: Bass, LowMid, HighMid, Treble, Beat, Snare
- effect browser: categories and favorites
- active effect quick controls
- performance pads
- profile save/load

Basic hides:

- raw shader errors
- full chain editor
- OSC/MIDI deep config
- per-param modulation matrix
- AI training details

Basic still exposes:

- effect selection
- 4-8 effect controls
- band selection
- beat bounce
- pads
- save/load
- strobe safety

### Pro: VJ Workbench

Pro screen sections:

- left: sources, AudioLink, band triggers
- center: program/preview state, active stack, pads
- right: inspector with tabs
- bottom: macros, beat quantize, MIDI/OSC, diagnostics

Pro inspector tabs:

- `Effect`
- `AudioLink`
- `Transform`
- `Color`
- `Glitch`
- `Pads`
- `Saving`
- `Debug`

### Settings Menu

Settings becomes a full tabbed modal/drawer:

- `General`: OBS websocket, autoconnect, HTTP port, startup mode
- `UI`: Basic/Pro default, density, theme, font scale, panel layout
- `AudioLink`: band sensitivity, thresholds, smoothing, beat/snare detection
- `Performance`: quality profile, FPS target, resolution scale, heavy effect warning
- `Strobe Safety`: max brightness, max flash rate, blackout, soft strobe
- `Pads`: pad count, banks, quantize, default pad mode
- `Effects`: favorites, hide heavy effects, reset current effect, reset all effect params
- `Saving`: autosave, profile path, import/export
- `Debug`: loaded DLL path, loaded data path, loaded Web UI path, state file path, shader errors

---

## New Effect: 2D Logo To 3D Object

Effect ID: `logo_extrude_3d`

Role: `generator`

Purpose: Take a 2D logo/image and render it as a thick 3D-looking object by extruding its alpha mask backward.

Basic controls:

- Logo Slot
- Size
- Depth
- Rotation
- Beat Bounce
- Audio React
- Material
- Position

Pro controls:

- position x/y/z
- scale x/y
- depth
- alpha threshold
- bevel width
- bevel softness
- rotation x/y/z
- rotation speed x/y/z
- beat bounce amount
- beat bounce decay
- audio depth amount
- audio rotation amount
- material roughness
- metallic/chrome amount
- rim light
- shadow amount
- glow amount
- quality samples

Technical approach:

- Use uploaded logo texture alpha as mask.
- Render front face from logo texture.
- Render multiple backward samples along pseudo-depth vector.
- Use alpha threshold to define solid shape.
- Use edge/normal approximation from alpha gradient for bevel lighting.
- Keep this shader-based first; do not introduce a mesh generator until shader extrusion proves insufficient.

---

## New Effect: Monochrome Text Ring Logo Tunnel

Effect ID: `halo_text_logo_tunnel`

Role: `generator`

Purpose: Procedural recreation of the provided GIF style using editable text rings, central 3D logo, monochrome glitch, and AudioLink motion.

Basic controls:

- Center Logo Slot
- Main Text
- Ring Count
- Intensity
- Speed
- Beat Bounce
- Glitch
- Mono/Invert

Pro controls:

- text ring 1 text
- text ring 2 text
- text ring 3 text
- text ring 4 text
- per-ring text mode: same, different, random, beat switch
- ring radius
- ring spacing
- ring perspective
- ring depth
- ring rotate speed
- ring scroll speed
- ring wobble
- center logo size
- center logo depth
- center logo rotation
- smoke amount
- scanline amount
- vertical barcode amount
- horizontal tear amount
- frame drop chance
- flash amount
- black crush
- white clip
- vignette
- glitch enable
- glitch seed speed
- audio ring pulse
- audio logo pulse
- audio glitch amount

The effect must reuse `logo_extrude_3d` logic conceptually so the center logo can be the user's 2D-to-3D logo.

---

## New Reusable Glitch Effect

Effect ID: `brutalist_scan_glitch`

Role: `filter` or `postprocess`

Purpose: Pull the GIF's glitch language into a reusable effect.

Controls:

- horizontal tear
- vertical scan columns
- frame smear
- black crush
- white clip
- noise smoke
- banded displacement
- text-safe mode
- beat hit amount
- snare hit amount

This effect can be embedded inside `halo_text_logo_tunnel` and also used independently in the effect chain.

---

## Effect Audit Program

The repo currently has 67 metadata files and about 485 parameters. Each effect must be audited with the same checklist:

- effect identity is normalized
- Basic controls exist and work
- Pro controls are grouped
- defaults are performance-safe
- AudioLink mapping is clear
- broken parameters are removed or fixed
- visual reference is attached in notes
- quality profile is defined
- save/load roundtrip passes

Categories:

- 3D
- audio_viz
- flash
- fractal
- geometric
- glitch
- particles
- plasma
- postprocess
- retro
- tunnel

---

## Verification Rules

Every implementation wave must run:

```powershell
powershell -ExecutionPolicy Bypass -File tools\validate_upgrade_contract.ps1
powershell -ExecutionPolicy Bypass -File tools\build_and_deploy_obs.ps1 -ObsDir 'F:\obs-studio' -AlsoDeploySdk
```

Additional checks:

- all `effects_meta/**/*.json` parse
- Web UI inline JS syntax passes `node --check`
- generated state file validates against schema
- OBS is restarted before visual testing
- Web UI is hard-refreshed with `Ctrl+F5`

---

## Out Of Scope For The First Build Wave

- full mesh generation from SVG paths
- cloud AI training
- replacing OBS rendering architecture
- adding a dependency-heavy frontend framework
- shipping online reference images into the plugin

These can come later after the state/settings/effect standard is stable.
