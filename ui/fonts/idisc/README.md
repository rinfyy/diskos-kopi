# iDisc theme fonts

Inter (The Inter Project Authors), licensed under the SIL Open Font License 1.1 (`licenses/OFL-1.1-Inter.txt`):

| File | Use |
|---|---|
| `Inter-Regular.otf` | body text, rows, captions |
| `Inter-SemiBold.otf` | titles, the Music title, buttons |
| `InterDisplay-SemiBold.otf` | the big lock-screen clock |

Taken unchanged from the Debian `fonts-inter` package. The firmware's LVGL fonts are made from these with
`python3 ui/tools/gen_idisc_fonts.py` (lv_font_conv 1.5.3) and are named `font_id_<face>_<px>`, so no Reserved Font
Name is used. Characters Inter lacks (CJK, symbols) fall back to the shared international chains.
