# VJLink UI Settings Effects Overhaul Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a consistent VJLink settings/effect standard, reliable backend-owned saving, a modern Basic/Pro Web UI, and the new 2D-logo extrusion plus monochrome text-ring logo tunnel effects.

**Architecture:** Normalize metadata first, then make the backend the canonical owner of state, then rebuild the Web UI around canonical state. New effects are added after the standard and saving path exist so their many settings persist correctly.

**Tech Stack:** OBS C plugin, OBS graphics `.effect` shaders, cJSON, PowerShell build/deploy scripts, single-file HTML/CSS/JS Web UI, OBS websocket vendor requests.

---

## File Structure

Create:

- `docs/effect_meta_v2_schema.md`: human-readable metadata schema.
- `tools/validate_effect_meta_v2.ps1`: validates normalized effect metadata.
- `tools/migrate_effect_meta_v2.ps1`: converts current metadata shape to v2 shape.
- `src/state/vjlink_state.h`: state model and public state functions.
- `src/state/vjlink_state.c`: load/save/apply state implementation.
- `effects/3d/logo_extrude_3d.effect`: 2D image alpha extrusion shader.
- `effects_meta/logo_extrude_3d.json`: metadata for logo extrusion.
- `effects/geometric/halo_text_logo_tunnel.effect`: GIF-inspired text-ring logo tunnel shader.
- `effects_meta/halo_text_logo_tunnel.json`: metadata for text-ring logo tunnel.
- `effects/glitch/brutalist_scan_glitch.effect`: reusable monochrome glitch shader.
- `effects_meta/brutalist_scan_glitch.json`: metadata for reusable glitch.

Modify:

- `CMakeLists.txt`: include new `src/state/*.c` file.
- `src/plugin-main.c`: initialize/shutdown state after effect registry and before HTTP/Web UI state usage.
- `src/vjlink_context.h`: add canonical state pointer and pending full-state apply flags.
- `src/vjlink_context.c`: initialize default values and state apply helpers.
- `src/rendering/effect_system.c`: parse v2 metadata while preserving legacy fallback.
- `src/rendering/effect_system.h`: expose v2 metadata fields needed by Web UI and state.
- `src/rendering/compositor.c`: apply saved chain params consistently.
- `src/rendering/band_effects.c`: apply saved band params consistently.
- `src/controls/websocket_handler.c`: add `GetFullState`, `SetFullState`, `SaveState`, `LoadState`, `ListProfiles`, `SaveProfileV2`, `LoadProfileV2`.
- `src/controls/http_server.c`: expose state/profile endpoints only through safe plugin data paths.
- `web-ui/vjlink-control.html`: replace localStorage-first profile logic with backend canonical state, add modern Basic/Pro layout and full settings menu.
- `tools/validate_upgrade_contract.ps1`: include metadata v2 and state schema checks.
- `tools/build_and_deploy_obs.ps1`: keep as the mandatory final deployment command.

Test:

- `tools/validate_effect_meta_v2.ps1`
- `tools/validate_upgrade_contract.ps1`
- JSON parse command for `effects_meta`
- Web UI JS `node --check`
- full build/deploy script

---

### Task 1: Lock The Metadata V2 Schema

**Files:**

- Create: `docs/effect_meta_v2_schema.md`
- Create: `tools/validate_effect_meta_v2.ps1`
- Modify: `tools/validate_upgrade_contract.ps1`

- [ ] **Step 1: Create the schema document**

Write `docs/effect_meta_v2_schema.md` with these sections:

```markdown
# VJLink Effect Metadata V2

Every effect metadata file uses `schema_version: 2`.

Required root fields:
- schema_version
- effect_id
- name
- category
- role
- performance_group
- quality_cost
- requires_input
- description
- controls
- audio_link

Allowed roles:
- generator
- filter
- postprocess
- overlay
- flash
- utility

Allowed quality_cost:
- low
- medium
- high
- extreme

Control fields:
- param
- label
- type
- default
- min
- max
- step
- group
- basic
- advanced
- audio_target
- save

Control groups:
- Main
- Transform
- AudioLink
- Color
- Texture/Input
- Glitch
- Performance
```

- [ ] **Step 2: Create the validator**

Write `tools/validate_effect_meta_v2.ps1` to parse all `effects_meta/**/*.json`, accept legacy files during migration, and fail v2 files that miss required fields.

Run:

```powershell
powershell -ExecutionPolicy Bypass -File tools\validate_effect_meta_v2.ps1
```

Expected:

```text
Effect metadata validation passed
```

- [ ] **Step 3: Wire validator into upgrade contract**

Modify `tools/validate_upgrade_contract.ps1` so it invokes `tools\validate_effect_meta_v2.ps1`.

Run:

```powershell
powershell -ExecutionPolicy Bypass -File tools\validate_upgrade_contract.ps1
```

Expected:

```text
VJLink upgrade contract passed.
```

---

### Task 2: Add Backend-Owned State V2

**Files:**

- Create: `src/state/vjlink_state.h`
- Create: `src/state/vjlink_state.c`
- Modify: `CMakeLists.txt`
- Modify: `src/plugin-main.c`
- Modify: `src/vjlink_context.h`
- Modify: `src/vjlink_context.c`

- [ ] **Step 1: Define state model**

Create `src/state/vjlink_state.h` with structs for:

- `vjlink_saved_param`
- `vjlink_saved_chain_slot`
- `vjlink_saved_audio_mapping`
- `vjlink_saved_band_slot`
- `vjlink_saved_pad`
- `vjlink_saved_profile`
- `vjlink_state`

Expose functions:

```c
bool vjlink_state_init(void);
void vjlink_state_shutdown(void);
bool vjlink_state_load_default(void);
bool vjlink_state_save_default(void);
bool vjlink_state_export_json(char **out_json);
bool vjlink_state_apply_json(const char *json);
const char *vjlink_state_get_path(void);
```

- [ ] **Step 2: Implement JSON load/save**

Create `src/state/vjlink_state.c` using cJSON. The default path is:

```text
data/obs-plugins/obs-vjlink/state/vjlink_state_v2.json
```

When the state file does not exist, create default state with:

- `schema_version = 2`
- `ui_mode = "basic"`
- `ai_mode = "off"`
- `strobe_safety = 1.0`
- empty chain
- empty pad banks

- [ ] **Step 3: Register state module in CMake**

Add `src/state/vjlink_state.c` and `src/state/vjlink_state.h` to `CMakeLists.txt`.

- [ ] **Step 4: Initialize state in plugin lifecycle**

In `src/plugin-main.c`:

- call `vjlink_state_init()` after preset/effect initialization
- call `vjlink_state_load_default()` before HTTP/Web UI starts
- call `vjlink_state_save_default()` during unload
- call `vjlink_state_shutdown()` during unload

- [ ] **Step 5: Build**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_and_deploy_obs.ps1 -ObsDir 'F:\obs-studio' -AlsoDeploySdk
```

Expected:

```text
Target    : F:\obs-studio
Target    : E:\Desktop\OBS VJ ADDON\obs-sdk
```

---

### Task 3: Add State WebSocket Requests

**Files:**

- Modify: `src/controls/websocket_handler.c`
- Modify: `web-ui/vjlink-control.html`

- [ ] **Step 1: Add backend requests**

In `src/controls/websocket_handler.c`, register:

- `GetFullState`
- `SetFullState`
- `SaveState`
- `LoadState`
- `ListProfiles`
- `SaveProfileV2`
- `LoadProfileV2`

Responses always include:

```json
{
  "success": true,
  "schema_version": 2
}
```

Failure responses include:

```json
{
  "success": false,
  "error": "specific message"
}
```

- [ ] **Step 2: Make Web UI bootstrap from backend**

In `web-ui/vjlink-control.html`, add:

```js
async function bootstrapCanonicalState() {
  const resp = await sendRequest('GetFullState', {});
  const data = resp?.responseData?.responseData || resp?.responseData || {};
  if (!data || data.success === false) return;
  applyCanonicalStateToUI(data);
}
```

Call it after websocket connection succeeds.

- [ ] **Step 3: Keep localStorage only as fallback**

Keep old `vjlink_profiles_v1` reading only behind an import button:

```js
function importLegacyLocalProfiles() {
  const legacy = JSON.parse(localStorage.getItem('vjlink_profiles_v1') || '{}');
  return saveLegacyProfilesToBackend(legacy);
}
```

---

### Task 4: Build The New Settings Menu

**Files:**

- Modify: `web-ui/vjlink-control.html`

- [ ] **Step 1: Replace the current settings drawer content**

Tabs:

- General
- UI
- AudioLink
- Performance
- Strobe Safety
- Pads
- Effects
- Saving
- Debug

- [ ] **Step 2: Add Debug path display**

Debug tab fields:

- Loaded DLL path
- Loaded data path
- Loaded Web UI path
- State file path
- OBS target path
- Shader errors

The values come from `GetFullState`.

- [ ] **Step 3: Add autosave status**

Add UI text:

- `Saved`
- `Saving...`
- `Unsaved`
- `Save failed`

Never rely on color alone; text must update.

---

### Task 5: Rebuild Basic And Pro Layouts

**Files:**

- Modify: `web-ui/vjlink-control.html`

- [ ] **Step 1: Basic layout**

Basic must show:

- Header
- AudioLink band strip
- Effect browser
- Active effect quick controls
- Pads
- Profile save/load

Basic must hide:

- chain editor
- raw shader errors
- MIDI deep editor
- OSC deep editor
- per-param modulation matrix

- [ ] **Step 2: Pro layout**

Pro must show:

- source/audio column
- effect stack
- inspector tabs
- pad editor
- macro strip
- diagnostics
- MIDI/OSC mapping

- [ ] **Step 3: Keep controls real**

For each Basic quick control, verify it sends `SetParam`, `SetBandParam`, `SetPalette`, `SetMacros`, or `SetFullState`.

---

### Task 6: Add Logo Extrude 3D Effect

**Files:**

- Create: `effects/3d/logo_extrude_3d.effect`
- Create: `effects_meta/logo_extrude_3d.json`
- Modify: `web-ui/vjlink-control.html`

- [ ] **Step 1: Create metadata**

`effects_meta/logo_extrude_3d.json` contains:

- `schema_version: 2`
- `effect_id: "logo_extrude_3d"`
- Basic controls: logo slot, size, depth, rotation, beat bounce, audio react, material, position
- Pro controls listed in the design spec

- [ ] **Step 2: Create shader**

Shader uniforms:

- `image`
- `resolution`
- `time`
- `bands`
- `beat_phase`
- `bpm`
- `logo_slot`
- `depth`
- `bevel`
- `size`
- `position_x`
- `position_y`
- `rotation_x`
- `rotation_y`
- `rotation_z`
- `beat_bounce`
- `audio_react`
- `material`
- `glow`
- `quality`

- [ ] **Step 3: Verify in OBS**

Run build/deploy and test:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_and_deploy_obs.ps1 -ObsDir 'F:\obs-studio' -AlsoDeploySdk
```

Expected OBS behavior:

- logo appears as thick pseudo-3D object
- depth changes visibly
- rotation works
- beat bounce can be disabled
- position and scale work

---

### Task 7: Add Halo Text Logo Tunnel Effect

**Files:**

- Create: `effects/geometric/halo_text_logo_tunnel.effect`
- Create: `effects_meta/halo_text_logo_tunnel.json`
- Modify: `web-ui/vjlink-control.html`

- [ ] **Step 1: Create metadata**

`effects_meta/halo_text_logo_tunnel.json` contains controls for:

- center logo slot
- main text
- ring count
- ring text 1-4
- ring speed
- ring radius
- perspective
- mono contrast
- invert
- smoke
- scanlines
- barcode columns
- glitch enable
- glitch amount
- beat pulse
- audio glitch

- [ ] **Step 2: Create shader**

Shader renders:

- black/white background
- central logo mask
- 2-4 perspective text rings
- per-ring different text values
- horizontal tearing
- vertical scan columns
- smoke/noise haze
- optional invert and strobe hits

- [ ] **Step 3: Match GIF movement**

Use the contact sheet at:

```text
E:\Desktop\OBS VJ ADDON\analysis\vjlink_gif_contact_sheet.png
```

Visual checkpoints:

- frame 0: dark, broken ring silhouettes
- frame 34-61: central bright logo region becomes dominant
- frame 82-116: text rings fill most of the screen
- frame 137-157: heavy scan/glitch and tunnel drift

---

### Task 8: Add Reusable Brutalist Scan Glitch

**Files:**

- Create: `effects/glitch/brutalist_scan_glitch.effect`
- Create: `effects_meta/brutalist_scan_glitch.json`

- [ ] **Step 1: Create metadata**

Controls:

- mix
- horizontal tear
- vertical scan columns
- frame smear
- black crush
- white clip
- noise smoke
- beat hit
- snare hit
- text-safe mode

- [ ] **Step 2: Create shader**

The shader modifies source input when present and falls back to procedural monochrome noise when no source is present.

---

### Task 9: Migrate Existing Effects To Metadata V2

**Files:**

- Create: `tools/migrate_effect_meta_v2.ps1`
- Modify: all `effects_meta/**/*.json`

- [ ] **Step 1: Write migration script**

Rules:

- `effect_id` comes from old `effect_id`, old `id`, or filename.
- each param gets `param`.
- display label comes from old `label`, old `name`, or `param`.
- `basic` is true for up to 8 important controls.
- `save` is true for all user-facing controls.
- hidden text char params remain advanced and saved.

- [ ] **Step 2: Run migration**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File tools\migrate_effect_meta_v2.ps1
```

Expected:

```text
Migrated 67 metadata files
```

- [ ] **Step 3: Validate**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File tools\validate_effect_meta_v2.ps1
```

Expected:

```text
Effect metadata validation passed
```

---

### Task 10: Effect Audit And Upgrade Pass

**Files:**

- Modify: `effects/**/*.effect`
- Modify: `effects_meta/**/*.json`
- Create: `docs/effect_audit_matrix.md`

- [ ] **Step 1: Create audit matrix**

Columns:

- effect_id
- category
- role
- basic controls
- pro groups
- audio targets
- visual reference
- quality cost
- status

- [ ] **Step 2: Audit all effects**

Count must match:

```text
67 metadata files
```

- [ ] **Step 3: Fix broken or weak effects first**

Priority list:

- `glitch_basic`
- `pixel_sort`
- `mono_outline`
- `bloom_pro`
- `holographic_foil`
- `flash_snare`
- `sphere_field`
- `vaporwave_sunset`
- `logo_runner`
- `type_animator`

---

### Task 11: Final Verification And Deployment

**Files:**

- Modify only files needed by failing checks.

- [ ] **Step 1: Validate upgrade contract**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File tools\validate_upgrade_contract.ps1
```

Expected:

```text
VJLink upgrade contract passed.
```

- [ ] **Step 2: Validate JSON**

Run:

```powershell
$ErrorActionPreference='Stop'
Get-ChildItem -Path effects_meta -Recurse -Filter *.json | ForEach-Object {
  $null = Get-Content -Raw -LiteralPath $_.FullName | ConvertFrom-Json
}
"JSON OK"
```

Expected:

```text
JSON OK
```

- [ ] **Step 3: Validate Web UI JavaScript**

Run:

```powershell
$ErrorActionPreference='Stop'
$html = Get-Content -Raw -LiteralPath 'web-ui\vjlink-control.html'
$matches = [regex]::Matches($html, '<script[^>]*>([\s\S]*?)</script>')
$js = ($matches | ForEach-Object { $_.Groups[1].Value }) -join "`n"
$tmp = Join-Path $env:TEMP 'vjlink-control-inline-check.js'
Set-Content -LiteralPath $tmp -Value $js -Encoding UTF8
node --check $tmp
```

Expected: no output and exit code 0.

- [ ] **Step 4: Build and deploy**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_and_deploy_obs.ps1 -ObsDir 'F:\obs-studio' -AlsoDeploySdk
```

Expected:

```text
Target    : F:\obs-studio
Target    : E:\Desktop\OBS VJ ADDON\obs-sdk
```

- [ ] **Step 5: Verify deployed hashes**

Run:

```powershell
Get-FileHash -Algorithm SHA256 `
  'build\Release\obs-vjlink.dll', `
  'F:\obs-studio\obs-plugins\64bit\obs-vjlink.dll', `
  'E:\Desktop\OBS VJ ADDON\obs-sdk\obs-plugins\64bit\obs-vjlink.dll'
```

Expected: all three DLL hashes match.

---

## Coverage Checklist

- Basic settings are included.
- Pro deep settings are included.
- Every effect gets a standard.
- Saving moves to backend-owned state.
- Existing localStorage profiles are migratable.
- 2D logo to 3D object is included.
- GIF-inspired text-ring logo tunnel is included.
- Glitch from the GIF becomes reusable.
- All effects get an audit and upgrade pass.
- Online VJ/AudioLink direction is reflected.
- Build/deploy to real OBS path is mandatory.

## Execution Recommendation

Use subagent-driven development:

- Worker 1: metadata schema, migration, validators
- Worker 2: backend state and websocket requests
- Worker 3: Web UI settings and Basic/Pro layout
- Worker 4: new shaders and metadata
- Worker 5: effect audit and visual polish

Each worker must avoid reverting unrelated edits and must list changed files in its final response.
