# materials: how each material should look

The look spec for the lit, textured renderer (the plan's R4a/R4b/R5), and the
detail tiles it reads. Written for whoever writes the shaders: every number here
is a starting point the Wave 4 checkpoint tunes, and the mood files
(`assets/<set>/<set>_mood.txt`) carry the per-scene values.

**Not a backdrop set**, so it breaks the `assets/README.md` set layout on
purpose: no `backdrop.txt`, no scene, no layers. It keeps the rest of the
convention -- one folder, every file prefixed with the folder name
(`materials_<name>.bmp`), every file reached from a manifest (a mood file's
`material <Name> texture=materials_<name>`, the full file stem), and a named generator:
`python tools/make_material_tiles.py` writes every BMP here and the contact
sheet `art_src/previews/materials.png`. Edit the script, not the BMPs.

## The rules every material keeps

- **The simulation's colour is the authority.** `Element::color` (base colour
  plus per-cell jitter, `src/physics/material.h`) is replay state, so the
  renderer never repaints a cell. Everything below is light, texture, tint and
  translucency applied on top of that colour, render-side only.
- **Three tones per material** (Rain World): highlight, neutral, shade. Neutral
  is the MATERIALS base colour unchanged. Highlight is where light and rim land;
  shade is where it doesn't. The shader should not lerp between them directly --
  `albedo x light` produces them -- but the ramp is the target the light model
  and grade are tuned to hit: a sunlit lip should land near the highlight hex
  and a cell deep in a wall near the shade hex. Shades lean **cool** and
  highlights **warm** (Backbone's teal shadow / warm practical split), except
  where noted.
- **Texture changes luminance only**: `albedo x (1 + s x (tile - 0.5))`, tile in
  0..1, `s` the mood's `strength`. Tiles are world-anchored, one texel per cell.
- **Rim** is the shader's edge light from the solid-mask gradient (fake normals
  from neighbours, Rain World's trick), tinted by whatever light reaches the
  edge. Once it ships, `apply_rim_light` comes out of `tools/pixel_art.py` and
  the albedos are regenerated so rims are not drawn twice (a later phase).

## Ramps

| Material | Highlight | Neutral (base) | Shade | Wet / special |
| :--- | :---: | :---: | :---: | :--- |
| Sand | `#E4CF9A` | `#C6A970` | `#86704F` | wet `#8C7650` |
| Wall (stone) | `#A39C90` | `#6F6A63` | `#3B3D45` | wet spec glint `#D8E4EA` |
| Wood | `#9A764E` | `#6B4E33` | `#3A2A20` | - |
| Charred | `#62534A` | `#3A3431` | `#1C1A1D` | ember `#FF6A1E` (emissive) |
| Water | `#8FD3DA` | `#2E7F96` | `#0F3442` | foam `#D8F0EE` |
| Oil | `#5E4C80` | `#2C2620` | `#100D10` | sheen `#3E7C78` <-> `#7A4E8E` |
| Steam | `#F0F5F7` | `#C4D2D8` | `#7F93A0` | - |
| Fire | `#FFE6A0` | `#F07A22` | `#A3301A` | all emissive |

## Per material

### Sand (powder)
- **Texture:** none from a tile. Powders keep their per-cell jitter, because a
  grain falling through a world-anchored pattern shimmers. Add sparse
  **sparkle**: a cell whose hash passes ~1-2% gets `spec`-weighted glints when a
  light is in front of it -- Backbone's wet-ground sparkle, Cast n Chill's
  sun-on-sand.
- **Wet:** sand with water above or beside it darkens toward the wet hex
  (about x0.7, slightly more saturated) and gains reflect. The noir mood's tint
  and spec stand in for "everything is rain-soaked".
- **Translucency / emissive:** opaque, none.
- **Rim:** 0.3-0.35, warm. A pile's top edge should catch the sun as a thin
  bright line; that line is most of what separates a dune from the backdrop.
- **Light:** fully diffuse. Shade cool, never black.

### Wall / stone (static) -- `materials_rock.bmp`
- **Texture:** `rock`: mottled grime (Rain World's grime noise), faint warped
  strata, broken cracks with a lit upper lip and a shadowed lower face, sparse
  bright mineral flecks. Strength 0.6 daylight, 0.7 noir.
- **Translucency / emissive:** opaque, none.
- **Rim:** 0.5-0.6, the strongest of the solids: terrain silhouettes are the
  composition. Rim takes the colour of the light reaching it -- gold at the
  tarn, sodium amber or neon in noir.
- **Light:** diffuse with a small `spec` (0.08 dry, 0.35 wet) that the mineral
  flecks multiply, so wet rock under a lamp glitters (Backbone). Deep inside a
  wall the colour should sink toward the cool shade hex with depth from the
  surface -- Rain World's palette-by-depth, done as a light falloff, not a
  repaint.

### Wood (static) -- `materials_wood.bmp`
- **Texture:** `wood`: horizontal planks 8 cells tall with seams, one staggered
  butt joint each, wandering grain lines, a knot in about half the planks and a
  lit top edge on each plank.
- **Translucency / emissive:** opaque, none.
- **Rim:** 0.4, warm.
- **Light:** diffuse, low spec (0.05; 0.2 wet). Warm albedo, so under sodium it
  glows and under the moon it nearly vanishes -- that is correct.

### Charred (static, burning) -- `materials_charred.bmp`
- **Texture:** `charred`: alligator crazing, domed scales split by cracks. Every
  crack texel is the tile's flat minimum, so `tile < 0.35` is a clean crack
  mask.
- **Emissive:** the crack mask glows ember `#FF6A1E`, pulsing slowly with a
  per-cell phase. Charred is the one static material that is a light source
  (it holds itself at 200 degrees), so the ember glow feeds the light pass too.
- **Rim:** 0.2 -- charcoal is matte; most of its edge light is its own glow.
- **Light:** diffuse, very dark; mostly lit by its embers and the fire it throws.

### Water (liquid) -- `materials_caustics.bmp`
- **Translucency:** the backdrop shows through. Alpha about 0.55 at the
  surface rising to about 0.9 at 16 cells deep (R4b caps alpha below 255).
- **Depth tint:** colour slides from neutral toward the shade hex with depth
  below the surface (scan up 16 cells or fewer). `depth` sets how far it gets.
- **Texture:** `caustics`, a bright filament net, world-anchored, sampled twice
  at two scroll speeds and combined with `min()` so it crawls. Strongest just
  under the surface, fading with depth; only where light reaches.
- **Surface:** a one-cell highlight line in the highlight hex on the top row,
  foam hex where `flow` or rise velocity is high.
- **Reflect:** Cast n Chill's mirrored reflection about the surface row,
  wobbled; `reflect` 0.5 daylight, 0.8 noir so lamps lie across the pond.
- **Spec:** high (0.6-0.85): lamps and the sun leave hot streaks on the surface.
- **Rim:** none -- the surface line replaces it.
- **Reference:** Cast n Chill (radiant above, muted below, the depth gradient,
  the reflection); Backbone (wet light streaks).

### Oil (liquid)
- **Translucency:** nearly opaque (alpha about 0.92).
- **Look:** black-violet. A thin-film **iridescent sheen** on the surface rows
  and where light hits: hue cycling between the two sheen hexes by a
  world-anchored noise and the view of the light, low saturation, never bright.
- **Spec 0.8-1.0**, reflect 0.3-0.45: oil is a dark mirror.
- **Rim:** none.
- **Reference:** Backbone's wet tarmac under neon.

### Steam (gas)
- **Translucency:** soft, alpha about 0.35, falling toward 0 at the plume's
  edges (fewer steam neighbours = more transparent), so puffs have no hard
  outline.
- **Light:** scatters: steam takes the colour of the light around it strongly,
  so a lamp behind steam makes a glowing halo. It should be lit even where the
  sun is occluded (it is a volume, not a surface).
- **Texture / rim / spec:** none.
- **Reference:** Backbone's volumetric fog and Rain World's mist.

### Fire (gas, emissive)
- **Emissive 1.0:** ignores the light model entirely (added after the multiply)
  and is never tinted or greyed by a mood -- it must never be missed.
- **Colour by life:** `ticks` (steps of flame left) ramps shade -> neutral ->
  highlight: young flame is the hot core, dying flame the deep red tip.
- **Flicker:** per-cell, from ticks and a hash, fast.
- **Bloom:** always above threshold; it is the main bloom source in every mood.
- **Light:** feeds the light pass as a warm point source (as `LightField` does
  today).
- **Reference:** Backbone's practical lights; Rain World's fire.

### Empty
Nothing: whatever is behind shows through.

## Tiles

| File | Read by | Pattern | Mean |
| :--- | :--- | :--- | :---: |
| materials_rock.bmp | Wall | grime, strata, cracks with lip, flecks | 128 |
| materials_wood.bmp | Wood | planks, seams, joints, grain, knots | 128 |
| materials_charred.bmp | Charred | crazing; cracks = flat minimum | 128 |
| materials_caustics.bmp | Water | caustic filament net | 128 |

All 64 x 64, seamless in both axes, greyscale stored as 24-bit BMP (r = g = b),
one texel per world cell (texel `(x mod 64, y mod 64)` for cell `(x, y)`).
Mid-grey is "no change". They are data maps, not colour art, so
`tools/validate_palette.py` will call them off-palette; that is expected.

## Grades

The `grade` LUT strips the moods name live in each scene's own folder
(`<set>_grade.bmp`, and `bg_tarn_noir_grade.bmp` for the noir mood) and are
written by `python tools/make_lut.py`, whose docstring has the strip layout and
the named grades: identity, daylight, golden_hour, noir.
