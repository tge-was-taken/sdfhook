# Curated states

Savestates made on the host PCSX2 and copied here, one per scene worth iterating on. Names are the
scene, so a dump directory names what it holds. `scripts/dump_scenes.sh` runs them all; pass names
to run a subset.

| state | what it is |
|---|---|
| `smt3_field_underground_facility` | field, the opening corridor. HUD, compass, a location label |
| `smt3_field_unknown_realm` | field, a second location. Same HUD shape, different world |
| `smt3_battle_unknown_realm` | battle, idle at the command panel. The heaviest 2D of the three |
| `smt3_menu_main` | the main menu |
| `smt3_title_screen` | title screen |
| `smt3_menu_loading_file_1` | the load-file prompt. **Needs a memory card**; cross enters the field |

The load prompt defaults to NO, so a blind `PCSX2HOOK_AUTOPRESS=cross` confirms it.
`PCSX2HOOK_NOMEMCARD` stalls this game's boot, so leave the card enabled.

## Which scene answers what

`smt3_menu_main` is the one to develop against. It draws **28143 triangle vertices a frame through
GIF** and leaves only 413 packets unread, where a field scene leaves 35481: the menu's geometry is
submitted directly, while the field's goes through VU1 and is not visible at this level at all. So
the menu is where the most of a frame is reachable.

The producers differ per scene, which is why more than one is kept. `DrwSprite2D` draws the field
and title HUDs and is not used by the menu at all; the menu draws through `cmpDrawTriG` (triangles,
477 verts/frame), `frCreateDrawFontPacket` (140), `fclDrawSpriteCore` (130) and `cmpDrawBoxG`
(tristrips, 23). Hooking one scene's producers proves nothing about another's.
