# Idiom — Sampler types must match texture compression

Material function `TextureSample` / `TextureObjectParameter` nodes have a `sampler_type` property. It MUST match the texture's `compression_settings` (and `srgb` flag) — Metal / SM6 fail compile with errors like:

> Sampler type is Linear Color, should be Alpha for T_xdhhdgq_4K_H

## The table

| Compression                  | sRGB | Correct `SAMPLERTYPE_*`     |
|------------------------------|------|------------------------------|
| `TC_DEFAULT`                 | on   | `COLOR`                      |
| `TC_DEFAULT`                 | off  | `LINEAR_COLOR`               |
| `TC_NORMALMAP`               | (n/a)| `NORMAL`                     |
| `TC_MASKS`                   | (n/a)| `MASKS`                      |
| `TC_GRAYSCALE`               | on   | `GRAYSCALE`                  |
| `TC_GRAYSCALE`               | off  | `LINEAR_GRAYSCALE`           |
| `TC_ALPHA`                   | (n/a)| `ALPHA`                      |
| any of above + virtual texture | (any) | matching `VIRTUAL_*` variant |

## Drive sampler from texture, not from guess

When you set a default texture on a parameter, **read the texture's properties first**:

```python
tex = easset.load_asset("/Game/Tex/T_Stone_Albedo")
comp = tex.get_editor_property("compression_settings")
srgb = tex.get_editor_property("srgb")

if comp == unreal.TextureCompressionSettings.TC_NORMALMAP:
    sampler = unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL
elif comp == unreal.TextureCompressionSettings.TC_MASKS:
    sampler = unreal.MaterialSamplerType.SAMPLERTYPE_MASKS
elif comp == unreal.TextureCompressionSettings.TC_ALPHA:
    sampler = unreal.MaterialSamplerType.SAMPLERTYPE_ALPHA
elif comp == unreal.TextureCompressionSettings.TC_GRAYSCALE:
    sampler = (unreal.MaterialSamplerType.SAMPLERTYPE_GRAYSCALE if srgb
               else unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE)
else:
    sampler = (unreal.MaterialSamplerType.SAMPLERTYPE_COLOR if srgb
               else unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)

ts_node.set_editor_property("sampler_type", sampler)
ts_node.set_editor_property("texture", tex)
```

## Megascan heightmap gotcha

Megascan heightmaps (`*_H` textures, e.g. `T_xdhhdgq_4K_H`) ship as `TC_ALPHA`, **not** `TC_GRAYSCALE`. They need `SAMPLERTYPE_ALPHA`. Defaulting to `LINEAR_COLOR` because they "look like a heightmap" trips the Metal compiler. Always read `compression_settings`.

## Virtual textures

If the texture has `virtual_texture_streaming = True`, use the `VIRTUAL_*` variant of whichever sampler the table picks (e.g. `SAMPLERTYPE_VIRTUAL_NORMAL`). Mix-up — using non-virtual sampler with a VT — also fails compile with a clear message.
