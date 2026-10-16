# Hoshimi Localify

- An xposed plugin for Idolypride
- Use this plugin at your own risk
- This repository is irrelevant to Idolm@ster and BandaiNamco

# Usage

- With Rooted Device: [LSPatch](https://github.com/LSPosed/LSPatch)

## Custom graphics

**Use custom graphics settings** expands the quality, resolution, shadow and AA controls. Preset buttons fill those controls together; manual changes show Custom unless all values match a preset. FPS and orientation are always visible and independent of this switch. Turning it off hides the custom controls, preserves their stored values and restores game graphics after a configuration refresh. Existing configurations retain their values, with new shadow and AA options defaulting to Game default.

| Preset | Game quality | Render scale | Maximum long edge | Character shadow resolution |
| --- | --- | --- | --- | --- |
| Low | 10 | 0.5 | 1024 | 512 |
| Medium | 20 | 0.67 | 1440 | 1024 |
| High | 30 | 0.85 | 2160 | 1024 |
| Very high | 40 | 1.0 | 4096 | 2048 |
| Ultra | 100 | 1.5 | 4096 | 4096 |

These are plugin presets, not copies of another game's preset table. The Quality dropdown offers Game default (0), Low (10), Medium (20), High (30), Very high (40) and Ultra/Recording (100). Quality 0 still allows separate custom resolution, shadow and AA settings. Out-of-range resolution input is not saved. The manager reports the game-requested `ThreeD` level outside override application so other game code does not see the injected recording level as the user preference.

**Ultra is experimental.** It selects the internal `Recording` quality (100), whose current game implementation uses a 5000 px render cap and requests Unity quality index 5. The inspected Android build only contains four Unity presets, so the override clamps the index to the highest preset actually available at runtime before applying it. The Ultra button also fills 1.5 render scale; it does not create another Unity preset. Custom resolution takes precedence over the recording cap, including the 4096 px safety limit. Missing required hooks/fields disable the quality override. Debug mode reports applied values as `GraphicQuality` entries. Recording-mode scene compatibility still requires device testing.

With custom graphics enabled, render scale controls the screen-sized 3D render target by 0.25–2.0, capped at a 256–4096 px long edge. The aspect ratio is preserved and dimensions are even. GPU dynamic scaling is disabled for these targets while the override is enabled. Explicit-size camera targets and unrelated UI resolution calculations are left to the game.

After refreshing the configuration, the quality manager reapplies changes on the game main thread, including resolution-only edits and switching off. Other camera targets update when the game next rebuilds them; reopen the 3D screen or restart if necessary. Higher resolutions consume more GPU time and render-target memory. Debug mode or image-resource logging reports the base and resulting sizes as `RenderResolution` entries in logcat.

### Antialiasing

AA offers **Default / Off / FXAA / SMAA (High)** as radio buttons, in one row when space permits and two rows on narrow screens. It is gated by the same custom graphics switch and defaults to Game default for existing configurations. The Ultra preset selects SMAA (High); all other preset buttons select Game default. Preset matching includes the expected AA mode. Refresh configuration to apply a selection; no render-target rebuild is required just for post-process AA.

The hook identifies the current `VLSRPCameraController` camera through the game controller registry and only overrides base-camera AA during URP camera-data initialization. It restores the component fields immediately afterward, leaving game state, UI cameras and auxiliary cameras untouched. Game default or custom graphics OFF therefore uses current game values on the next rendered frame. Missing bindings skip the override. FXAA/SMAA are not forced into scenes with post-processing disabled. MSAA is not overridden. Debug mode reports effective camera-data values as `AntiAliasing`; temporary post-process observation hooks are not included.

### Character shadow resolution

The selector next to Quality offers **Game default / 512 / 1024 / 2048 / 4096**. Existing configurations default to Game default. Preset buttons fill the shadow resolution listed above; preset matching includes this value. Low through Very high follow the original resolutions observed for each game quality level. Ultra deliberately raises the original Recording resolution from 2048 to the tested 4096 override. Selecting a quality from the Quality dropdown alone does not overwrite the separate shadow selection.

The override changes only the resolution argument passed to `ActorShadowPass.UpdateShadowData` for the current main 3D camera. It does not force shadow support or modify the game settings object. Game default and custom graphics OFF pass the original resolution through; unsupported values, missing camera metadata, or resolutions above the device texture limit also fall back to the game. Refresh configuration to apply changes. 4096 has four times the texture pixels of 2048 and may increase GPU and memory load. Temporary shadow observation hooks are not included.

# Special Thanks

#### Chinosk6
- [gakuen-imas-localify](https://github.com/chinosk6/gakuen-imas-localify) (Original Code)
#### Vilebbit
- [HoshimiToolkit](https://github.com/Vilebbit/HoshimiToolkit)
- [SolisClient](https://github.com/Vilebbit/SolisClient)

