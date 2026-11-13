# Editor rendering gaps

## Ranked gaps

1. **Terrain detail textures** (47 of 60 worlds, every mission and MP map). The editor draws the colour map and lightmap only. The game multiplies two tiled detail textures (picked per pixel by the colour map's alpha) and two micro-detail textures over it. All data is in MATerrain plus Terrain/*.tif in the level archive. Painting textures = painting the colour map's alpha.
2. **Water material** (23 worlds). The water model's `<material_water>` has a wave texture, a sky reflection map, wave/distort factors, fresnel and deep/shallow colours. The editor draws a flat translucent tinted slab.
3. **Lighting and time of day** (all worlds; 34 are night). Ambient and directional colours and IsNightTime are ignored; everything is lit like noon.
4. **Fog** (all worlds). FogStart/End/Color and the height band are edited on the Map tab but never drawn.
5. **Model materials beyond the diffuse texture**: no reflection, animated frames, detail or emissive (self-illum); glass/chrome panels without a base texture show as white blocks.
6. **MAEffect fires/smoke** (6 worlds; Mission_02 has 11 oil fires on the crash scar): orange boxes in the editor.
7. **Culture scatter** (22 worlds): grass/bush/rock scatter generated from the MATerrainInfo culture map; not drawn (fades out by about 75 m in game).
8. **Rain and point lights**: rain in 5 worlds, MAPointLight in 27; previews missing, low priority.

Not gaps: craters and weapon scorch decals are made at run time (MAModSubObjectDecalStamp/DecalObject). The sky dome, buildings, the crashed dropship, rubble chunks and road tiles already render correctly.

## How the comparison was run

- One game at a time, quit after each.
- Data: `inspect_wdf.parse_wdf` over common.mgf `Missions/*.wdf` and `Multiplay/*.wdf`; level archives listed with `inspect_mgf.Mgf`.

