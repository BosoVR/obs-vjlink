# OBS-VJLink

Audio-reactive VJ visuals plugin for OBS Studio. Turn OBS into a real-time visual performance tool for DJs, VJs, and live streamers.

Inspired by [AudioLink](https://github.com/llealloo/vrc-udon-audio-link) for VRChat.

---

Hey, I'm **Boso** (BosoVR on VRChat) - DJ and visual artist. You can find my mixes on [SoundCloud](https://soundcloud.com/bosovr). I DJ in VRChat and needed a better VJ system because I was never fully satisfied with just using images or GIFs as visuals. So I built this plugin with the help of AI. Hope this helps all my fellow DJs out there - if you want to work with it or collaborate, add me on Discord: **bosovr**

---

## Features

- **53 Shader Effects** across 11 categories (Tunnels, Plasma, Particles, Fractals, Geometric, Glitch, Retro, 3D, Audio Viz, Post-Processing, Flash/Strobe)
- **Real-Time Audio Analysis**: 4-band FFT (Bass, Low-Mid, High-Mid, Treble), BPM detection with phase tracking, onset detection, chronotensity
- **Per-Band Effect Triggers**: Assign flash/strobe effects to specific frequency bands with threshold + intensity control
- **Logo System**: Load custom logo images (PNG/JPG/GIF) into effects like Beat Blocks, Logo Runner, Logo Pulse, Strobe Grid
- **Web Control Panel**: Full browser-based UI with live audio visualization, effect selection, parameter sliders, per-band configuration
- **Effect Chain**: Stack multiple effects with blend modes (Normal, Add, Multiply, Screen)
- **Transparent Background**: Layer VJ visuals over other OBS sources with automatic luminance-to-alpha conversion
- **Preset System**: 12 built-in presets, JSON-based save/load, parameter animation (LFO, audio-band, beat-envelope, sequencer)
- **3D Engine**: Raymarching/SDF effects, procedural meshes, orbit camera
- **GPU Particles**: 16,384 particles with ping-pong simulation, audio-reactive forces
- **Video Wall**: Region-based sources for multi-display/LED wall setups
- **Debug Overlay**: Real-time band levels, BPM, FPS display
- **Shader Hot-Reload**: Edit effects at runtime without restarting OBS
- **WebSocket API**: 18 vendor request types for external control (MIDI controllers, Stream Deck, custom apps)
- **OBS Hotkeys**: Next/Prev preset, tap BPM, blackout, preset slots 1-10

## OBS Source Types

| Source | Type | Purpose |
|--------|------|---------|
| **VJLink Compositor** | Video Input | Main visual canvas with effect chain, per-band effects, logo |
| **VJLink Audio Analyzer** | Audio Filter | Attach to any audio source for FFT + BPM analysis |
| **VJLink Effect Filter** | Video Filter | Apply VJ shader effects to any OBS source |
| **VJLink Video Wall** | Video Input | Display a sub-region of the compositor for multi-display |

## Quick Start

1. Download the latest release ZIP from [Releases](https://github.com/vjlink/obs-vjlink/releases)
2. Extract into your OBS Studio directory (merge the `obs-plugins` and `data` folders)
3. Restart OBS Studio
4. Add **VJLink Audio Analyzer** filter to your Desktop Audio source
5. Add **VJLink Compositor** as a source in your scene
6. Select an effect from the properties panel
7. Open the Web UI at `http://localhost:8088` for full control
8. Play music and watch the visuals react

## Web Control Panel

Open `http://localhost:8088` in any browser while OBS is running.

- Live audio band visualization with spectrum display
- Effect browser with category filters
- Per-effect parameter sliders with audio band mapping
- Per-band effect configuration (Bass/Low-Mid/High-Mid/Treble)
- Logo path input
- Transparent background toggle
- Debug overlay toggle
- Preset navigation
- Blackout button
- Band sensitivity controls

## Effect Catalog (53 Effects)

### Tunnels (4)
Classic Tunnel, Hex Tunnel, Galaxy Tunnel, DNA Helix

### Plasma (3)
Plasma Classic, Lava Lamp, Liquid Metal

### Particles (3)
Fire Emitter, Galaxy Spiral, Particle Storm

### Fractals (3)
Mandelbrot Zoom, Julia Set, IFS Fractal

### Geometric (7)
Kaleidoscope, Voronoi, Sacred Geometry, **Beat Blocks**, **Beat Rings**, **Logo Runner**, **Logo Pulse**

### Glitch (4)
RGB Split, Datamosh, Pixel Sort, ASCII Art

### Retro (3)
Matrix Rain, CRT Scanlines, VHS Glitch

### 3D (3)
Sphere Field, Torus Knot, Metaballs

### Audio Visualization (4)
Waveform Bars, Spectrum Radial, Beat Flash, **Laser Scan**

### Post-Processing (3)
Color Grade, Feedback, Displacement

### Flash/Strobe (9)
Hard Strobe, Color Strobe, Kick Flash, Snare Flash, Invert Strobe, Radial Flash, Strobe Gate, Flash Shake, **Strobe Grid**

## Per-Band Effects

Assign any flash/strobe effect to a specific audio frequency band:

| Band | Frequency Range | Typical Use |
|------|----------------|-------------|
| Bass | 23-234 Hz | Kick drums, sub bass |
| Low-Mid | 234-984 Hz | Snares, vocals |
| High-Mid | 984-3984 Hz | Hi-hats, synths |
| Treble | 3984-12000 Hz | Cymbals, air |

Each band has configurable **threshold** (when to trigger) and **intensity** (how strong). Effects only fire when the band energy exceeds the threshold.

## Logo Effects

Load a custom logo image (PNG/JPG/GIF with transparency) via the OBS properties or Web UI:

| Effect | Logo Feature |
|--------|-------------|
| **Beat Blocks** | Logo in 4 corners, pulses on beat |
| **Logo Runner** | Logo slides across screen (L/R, diagonal, random) |
| **Logo Pulse** | Logo center-screen, scales up on beat with glow |
| **Strobe Grid** | 3 modes: logo shape lights up / dark silhouette / original colors |

## Building from Source

### Requirements
- Windows 10/11
- Visual Studio 2022 (Build Tools or full IDE)
- CMake 3.20+
- OBS Studio 28+ (with headers/libs)

### Build Steps

```bash
git clone https://github.com/vjlink/obs-vjlink.git
cd obs-vjlink
mkdir build && cd build
cmake -G "Visual Studio 17 2022" -A x64 -DOBS_DIR="path/to/obs-studio" ..
cmake --build . --config Release
```

The DLL and data files are automatically copied to the OBS plugin directory after build.

### Manual Installation

Copy from the `build/Release/` directory:
- `obs-vjlink.dll` to `<OBS>/obs-plugins/64bit/`
- `effects/`, `effects_meta/`, `presets/`, `web-ui/`, `locale/`, `textures/` to `<OBS>/data/obs-plugins/obs-vjlink/`

## WebSocket API

Requires [obs-websocket](https://github.com/obsproject/obs-websocket) v5+ (built into OBS 28+).

### 18 Request Types

| Request | Description |
|---------|-------------|
| `SetEffect` | Set active effect by ID |
| `SetPreset` | Apply preset by name |
| `SetParam` | Set shader parameter value |
| `NextPreset` / `PrevPreset` | Navigate presets |
| `TapBPM` | Manual beat tap |
| `Blackout` | Toggle blackout |
| `GetState` | Get full plugin state (bands, BPM, beat, presets) |
| `SetBandEffect` | Configure per-band effect (effect, threshold, intensity) |
| `GetBandEffects` | Get per-band configuration |
| `SetSourceTrigger` | Configure OBS source audio triggers |
| `GetSourceTriggers` | Get source trigger configuration |
| `SetMediaLayer` | Configure media overlay layer |
| `GetMediaLayers` | Get media layer configuration |
| `GetSceneSources` | List OBS scene sources |
| `SetSensitivity` | Set per-band sensitivity multipliers |
| `SetLogo` | Set logo image path |
| `SetTransparentBg` | Toggle transparent background |

## Audio Texture Format

Audio data is stored in a 512x4 RGBA32F GPU texture, accessible in all shaders as `audio_tex`:

| Row | Content |
|-----|---------|
| 0 | FFT Spectrum (512 bins) |
| 1 | PCM Waveform (512 samples) |
| 2 | Bands + Beat Phase + BPM + RMS + Chronotensity + Onset + LFOs |
| 3 | Band energy history (512 frames rolling) |

## Shader Development

Every effect receives these standard uniforms:

```hlsl
uniform texture2d audio_tex;    // 512x4 audio data
uniform texture2d image;        // Input texture (filter mode)
uniform texture2d logo_tex;     // User logo image
uniform texture2d prev_tex;     // Previous frame (feedback)
uniform float2    resolution;   // Output resolution
uniform float     time;         // Elapsed seconds
uniform float     beat_phase;   // 0.0-1.0 beat ramp
uniform float     bpm;          // Detected BPM
uniform float4    bands;        // x=bass y=lowmid z=highmid w=treble
uniform float     band_activation; // Per-band trigger level (0-1)
uniform float     has_input_source; // 1.0 in filter mode
```

Effects are HLSL `.effect` files in `effects/<category>/`. Metadata is defined in JSON files in `effects_meta/`. Shaders are automatically hot-reloaded when modified.

## Architecture

```
[Audio Source] -> [Audio Analyzer Filter] -> [Audio Engine (KissFFT)]
                                                    |
                                              [Audio Texture 512x4]
                                                    |
                    +-------------------------------+
                    |                               |
             [Compositor Source]              [Effect Filter]
              - Effect chain                  - Per-source shader
              - Per-band effects              - Same audio_tex
              - Logo system
              - 3D engine / Particles
              - Feedback buffer
              - Luma-alpha transparency
              - Debug overlay
                    |
             [Render Target]
                    |
              +-----+-----+
              |     |     |
           [Wall] [Wall] [Wall]
                    |
             [Web UI :8088]
              - Live control
              - Audio viz
              - Parameter mapping
```

## Third-Party Libraries

- [KissFFT](https://github.com/mborgerding/kissfft) - BSD-3-Clause License
- [cJSON](https://github.com/DaveGamble/cJSON) v1.7.19 - MIT License

## License

**Free for personal use** (DJing, VJing, streaming, events). Commercial license required if you integrate VJLink into software that is sold. See [LICENSE](LICENSE) for details. Contact **bosovr** on Discord for commercial licensing.
