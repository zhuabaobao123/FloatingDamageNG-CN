# Contributing

Bug reports with logs are contributions too — the issue templates ask for the
right files. For code:

## Building

```powershell
git clone --recursive https://github.com/alandtse/FloatingDamageNG
cd FloatingDamageNG
xmake
```

Requires VS 2022 and [xmake](https://xmake.io). The sister-repo client APIs
(imgui-vr-helper, SKSE Menu Framework, devbench) are pulled at build time from
the pins in `xmake-pkgs/`; CommonLibVR-NG is the `lib/commonlibsse-ng`
submodule. Set `SkyrimVRPath` or `SkyrimVRPluginTargets` to auto-deploy the
DLL after each build. Install the pre-commit hooks: `pre-commit install`.

## The rules that bite

- **Threading.** The engine writes actor values from job threads and Papyrus
  script threads. Hook entry points in `Hooks.cpp`/`Capture.cpp` may only
  capture POD into the raw ring — never call actor methods, never enumerate
  active effects, never touch ImGui there. All processing happens in
  `Capture::ProcessQueued()` on the render tick. Two CTDs taught us this;
  don't relearn it.
- **Multi-runtime.** One DLL serves SE/AE/VR. Vfunc slots and offsets can
  differ per runtime (`HandleHealthDamage` is 0x104 SE/AE but 0x106 VR);
  resolve with `REL::Module::IsVR()` / `RELOCATION_ID` / `OFFSET`, and count
  _all_ bases with vtables when indexing a `VTABLE` array — `TESObjectREFR`
  contributes four before Actor's own interfaces.
- **VR rendering.** Submit world quads in raw Skyrim world coordinates; the
  helper converts at submit time. Never draw screen-space overlays in VR.

## Commits and PRs

[Conventional Commits](https://www.conventionalcommits.org) — releases are cut
automatically by semantic-release from commit types, so pick the type for its
version impact (`feat` = minor, `fix`/`perf` = patch, `!` = major; `build`/
`ci`/`docs`/`refactor`/`chore` don't release). PRs are squash-merged; the PR
title becomes the released commit message and is lint-checked for the same
format.

## License

Contributions are accepted under GPL-3.0-or-later WITH
LicenseRef-Modding-Exception (see COPYING and EXCEPTIONS.md), except
contributions to `api/`, which are accepted under LGPL-3.0-or-later (see
api/COPYING.LESSER).
