# FloatingDamageNG

Floating damage numbers for Skyrim. One DLL for **SE, AE, and VR**, with true
world-anchored 3D numbers in VR and a built-in combat analytics log.

**Download:** [Nexus Mods](https://www.nexusmods.com/games/skyrimspecialedition/mods/184159/) ·
**Source:** [github.com/alandtse/FloatingDamageNG](https://github.com/alandtse/FloatingDamageNG) (GPL-3.0 with modding exception)

## What you see

- Damage numbers over whoever takes the hit, colored by what caused it:
  physical (crimson), fire, frost, shock, poison, untyped magic, plus healing
  (green `+N`). An enchanted-weapon swing shows two numbers (the physical hit
  and the enchantment payload) because the engine applies them separately and
  so do we. Frost and shock also apply their stamina/magicka bleed, which can
  display too (off by default).
- **Crit / sneak / bash / block** styling from the engine's real hit flags,
  and a mitigation subtext that says what actually happened: `104 (-45
armor)`, `(-30 blocked)` with the true blocked amount, or `(-20 resisted)`,
  sized by how much of the hit was mitigated.
- **HEADSHOT** tags on bow and crossbow hits, resolved from the engine's hit
  position against the victim's skeleton. Works in vanilla, and stays
  accurate under locational-damage mods (whose multipliers are already
  reflected in the number; an implied `x2.5` shows when a hit exceeds its
  normal baseline).
- Numbers follow their target while loaded: a fleeing enemy under Flames
  carries its counting-up burn total with it; your self-heal rides along as
  you move. Damage-over-time ticks pool into one live-updating number instead
  of spam.
- Player-received damage and healing: over your head in third person, pinned
  to a configurable screen spot in first person (flat), anchored ~1 m ahead at
  chest height in VR.
- **Data-driven motion.** An effect is a set of physics values (rise speed,
  gravity, launch speed, damping, spread pattern, per-hit angle); a preset is
  just saved values. Float is the built-in default; Arc, Radial, Fireworks,
  and Freeze/Accelerate/Bound/Drop ship as editable JSON in
  `Data/SKSE/Plugins/FloatingDamageNG/Presets`. The current effect saves back
  to a JSON file to share, and you can edit or drop in your own. Presets carry
  an optional description and attribution shown in the picker.
- **Per damage type.** Any damage type can take its own motion preset and
  font, the same way it takes its own color (e.g. fire sprays while frost
  drifts).
- **Fonts.** Chosen in-game from the TTF/OTF files in a `Fonts` drop-in folder
  and the Windows font directory, globally or per type. A change applies on
  the next game start (the glyph atlas is baked once per context).
- **Sizing.** A base pixel size, a multiplier, and log-scaled growth for large
  hits, plus distance controls (a reference distance, and a flat shrink floor
  or a VR growth cap) so distant hits stay readable at range. Optional `1.2k`
  abbreviation and squash-and-stretch. A live preview spawns sample numbers on
  a console-selected target (`prid`, else the player); the spawn point can be
  offset up, toward you, or sideways.
- **Origin marker** shows whose fight a number is: a colored text outline,
  underline, box, or none (your hits black, damage you take red, follower
  damage blue, bystander fights gray; all themable, with a live preview in the
  menu). Follower and NPC-vs-NPC numbers render smaller and are distance-culled.
- **Combat analytics** (optional): each fight is logged as a session
  (per-combatant damage dealt/taken, healing, crits, time-to-die, who fled)
  with real, active, and peak DPS. Per-combatant drill-downs show damage **by
  weapon/spell** and **by target** as kind-colored meter bars, a **resist
  profile** (how much of each element that enemy shrugged off), and a **death
  recap** (who landed the killing blow and the hits leading up to it). The
  stats browser (SKSE Menu Framework) has DPS graphs, click-to-sort tables,
  and a name filter; a live DPS readout shows during combat (top-right on
  flat, head-locked HUD plane in VR). Sessions append to a plain-text log, and
  opt-in **JSONL/CSV exports** feed pandas/jq/Excel.

## VR

In VR the numbers are **real 3D billboards at the target's world position**,
rendered through [ImGuiVRHelper](https://github.com/alandtse/imgui-vr-helper)'s
world-quad API with per-pixel depth occlusion. They are not drawn on the HUD
plane: flat screen-space overlays in a headset cause stereo convergence
mismatch (the number sits at HUD depth while the enemy is meters away), which
is uncomfortable at best. If ImGuiVRHelper is not installed, numbers stay off
in VR rather than falling back to a flat overlay.

## How it compares

All three mods put numbers over enemies. The differences are in how the damage
data is obtained and how the text is rendered. (Floating Damage details are
from reading its published v0.4 source; Modern Floating Damage from its public
description.)

|                      | **FloatingDamageNG**                                                                           | [Floating Damage](https://www.nexusmods.com/skyrimspecialedition/mods/14332)                           | [Modern Floating Damage](https://www.nexusmods.com/skyrimspecialedition/mods/170076)           |
| -------------------- | ---------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------- |
| Runtimes             | SE + AE + VR, one DLL                                                                          | SE/AE                                                                                                  | SE/AE                                                                                          |
| VR                   | World-anchored 3D billboards                                                                   | No (long-requested)                                                                                    | No (renderer is flat-screen)                                                                   |
| Damage data          | Engine damage + magic-effect pipeline hooks: true per-application amounts, source, caster      | Polls Health/Magicka/Stamina deltas per frame; must estimate and subtract regen; no damage source info | Hooks the damage pipeline; splits physical vs enchant with per-source colors                   |
| Component splitting  | Yes (physical + each effect separately)                                                        | No (net stat change only)                                                                              | Yes                                                                                            |
| Mitigation breakdown | armor / blocked / resisted amounts, from live `HitData` + resist values                        | No                                                                                                     | Not advertised                                                                                 |
| Hit location         | HEADSHOT etc. from engine impact nodes, regex-configurable                                     | No                                                                                                     | Not advertised                                                                                 |
| Renderer             | Dear ImGui, TTF fonts (D3D11; VR via ImGuiVRHelper)                                            | Scaleform SWF (Flash)                                                                                  | [PrismaUI](https://www.nexusmods.com/skyrimspecialedition/mods/170076) (embedded web/CEF view) |
| Combat analytics     | Sessions, DPS, drill-downs, resist profiles, death recaps, JSONL/CSV export, optional MCP/REST | No                                                                                                     | No                                                                                             |
| Extra dependencies   | None required on flat; ImGuiVRHelper for VR                                                    | None                                                                                                   | PrismaUI                                                                                       |
| Source code          | Open (GPL-3.0 + modding exception, this repo)                                                  | Only an early version (v0.4) was ever published; the author is no longer active                        | Closed                                                                                         |

If you play flat-screen only and want the web-styled visuals, Modern Floating
Damage is a good mod; the honest difference there is renderer taste
(embedded browser vs ImGui), analytics, and the mitigation/locational detail.
If you play VR, this is currently the only one of the three that renders in
world space at all.

This is also the only floating-damage mod that is open source in its current
form. That matters in practice: when a game update breaks plugins, anyone can
rebuild against a new Address Library instead of waiting on a single author,
the damage hooks are auditable by other mod developers for compatibility work,
and the project can be forked if it is ever abandoned.

## Requirements

- **SE/AE**: [SKSE64](https://skse.silverlock.org/), [Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- **VR**: SKSE VR, [VR Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/58101),
  [ImGuiVRHelper](https://github.com/alandtse/imgui-vr-helper) ≥ 1.5
- Optional: [SKSE Menu Framework 3](https://github.com/alandtse/SKSE-Menu-Framework-3)
  for the in-game settings panel (color pickers, sliders) and combat-stats
  browser. Without it, everything is configurable in the INI.
- Optional: [devbench](https://github.com/alandtse/devbench): when installed
  _and_ enabled (`bEnableDevBench`, off by default), combat stats are queryable
  over local MCP/REST for external tools.

## Configuration

`Data/SKSE/Plugins/FloatingDamageNG.ini` holds every filter, color, style,
threshold, and motion setting, with comments. With SKSE Menu Framework
installed the same settings are editable in-game with a live style preview and
saved back to the INI. Combat session reports append to
`FloatingDamageNG-combat.log` next to your SKSE logs; optional structured
exports write `FloatingDamageNG-sessions.jsonl` (one JSON object per session,
full drill-down data) and `FloatingDamageNG-combatants.csv` (one row per
combatant, spreadsheet-ready). All three rotate at 5 MB.

## Building

```powershell
git clone --recursive https://github.com/alandtse/FloatingDamageNG
cd FloatingDamageNG
xmake
```

Sister-repo client APIs (imgui-vr-helper, SKSE Menu Framework, devbench) are
pulled at build time from pinned upstream commits via the local package repo
in `xmake-pkgs/`. Set `SkyrimVRPath`, `SkyrimVRPluginTargets`, or
`SkyrimPluginTargets` to auto-deploy after each build.

## API for other mods

Other SKSE plugins can show their own popups through FloatingDamageNG and
claim a hit so it isn't shown twice, for example a locational-damage mod
replacing the HEADSHOT tag with its own. See [api/README.md](api/README.md).

## Credits

- Display architecture based on [Floating Subtitles](https://github.com/powerof3/FloatingSubtitles)
  (powerof3; VR fork by alandtse).
- Hit-data capture patterns from [KillFeed](https://github.com/powerof3/KillFeed)
  (powerof3; GPL-3.0 with modding exception).
- The original [Floating Damage](https://www.nexusmods.com/skyrimspecialedition/mods/14332)
  by [Felisky384](https://www.nexusmods.com/profile/Felisky384) defined the
  genre; this project reimplements the idea on a
  different capture and rendering architecture.

## License

GPL-3.0-or-later WITH LicenseRef-Modding-Exception. See [COPYING](COPYING) and
[EXCEPTIONS.md](EXCEPTIONS.md).

The client API in [api/](api/) is licensed separately under
LGPL-3.0-or-later (see [api/COPYING.LESSER](api/COPYING.LESSER)), the same
split imgui-vr-helper uses. A mod that copies those two files to talk to
FloatingDamageNG is bound only by the LGPL for them, and does not have to adopt
the GPL for its own code; changes to the API files themselves stay LGPL.
Nothing else in this repository is covered by the LGPL.
