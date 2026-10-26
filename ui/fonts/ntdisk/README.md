# NTDisk theme fonts

All three are licensed under the SIL Open Font License 1.1 (texts in `licenses/`):

| File | Family | Source | Licence |
|---|---|---|---|
| `Doto-Black.ttf` | Doto, weight 900 (dot-matrix: clock, big numbers) | Doto Project Authors, via @fontsource/doto 5.3.0 (Latin subset) | `licenses/OFL-1.1-Doto.txt` |
| `SpaceMono-Regular.ttf` | Space Mono 400 (text, "Dot + Mono" and "Mono") | The Space Mono Project Authors, via @fontsource/space-mono 5.3.0 (Latin subset) | `licenses/OFL-1.1-SpaceMono.txt` |
| `SpaceGrotesk-Regular.ttf` | Space Grotesk 400 (text, "Dot + Sans") | Florian Karsten, via @fontsource/space-grotesk 5.3.0 (Latin subset) | `licenses/OFL-1.1-SpaceGrotesk.txt` |

The WOFF files were unwrapped to TTF with fontTools; no glyphs were changed. The firmware's LVGL fonts are made from
these with `python3 ui/tools/gen_ntdisk_fonts.py` (lv_font_conv 1.5.3) and are renamed `font_nt_<face>_<px>`, so no
Reserved Font Name is used. Characters these faces lack (CJK, symbols) fall back to the shared international chains.
