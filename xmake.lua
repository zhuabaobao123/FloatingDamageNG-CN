-- FloatingDamageNG xmake build script
-- SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception

set_xmakever("2.8.2")

set_config("rex_ini", true)

includes("lib/commonlibsse-ng")

set_project("FloatingDamageNG")
set_license("GPL-3.0")

local version = "1.5.1"
local ver = version:split("%.")
set_version(version)

set_languages("c++23")
set_warnings("allextra")
set_policy("package.requires_lock", true)
add_rules("mode.debug", "mode.releasedbg")
set_defaultmode("releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- Local package repo: pins the sister-repo client APIs (imgui-vr-helper,
-- SMF3, devbench) to exact upstream tags/commits so they are pulled at build
-- time instead of vendored. Cached in xmake's package cache; bump the pin in
-- xmake-pkgs/packages/** to update (or point the pin at a local commit for
-- testing).
add_repositories("fdng-pkgs xmake-pkgs")

-- Runtime dependencies. Pinned versions resolved into xmake-requires.lock.
add_requires("imgui", { configs = { dx11 = true, win32 = true } })
add_requires("simpleini") -- Data/SKSE/Plugins/FloatingDamageNG.ini
add_requires("nlohmann_json") -- imgui-vr-helper client SDK + devbench tool payloads
add_requires("imgui-vr-helper-api 1.5.2") -- VR world-quad client API (LGPL, source-form)
add_requires("skse-menu-framework-api 3.7.0") -- optional in-game config UI host (LGPL header)
add_requires("devbench-api 1.5.0") -- optional MCP/REST stats exposure (MIT)

-- The SMF-hosted config UI lives in its own PCH-free static lib: the SMF
-- client header's cimgui-style typedefs cannot coexist with the real imgui.h
-- that the main target's PCH pulls in.
target("FloatingDamageNG-UI")
set_kind("static")
set_warnings("all") -- third-party SMF/CommonLib headers compile in this TU without the PCH's suppressions
add_deps("commonlibsse-ng")
add_packages("skse-menu-framework-api")
add_defines("_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING") -- SMF header uses std::wstring_convert
add_defines("UNICODE", "_UNICODE") -- SMF header passes a wide string to GetModuleHandle
add_files("src/UI.cpp")
add_includedirs("src")
target_end()

target("FloatingDamageNG")
add_deps("commonlibsse-ng", "FloatingDamageNG-UI")
add_packages("imgui", "simpleini", "nlohmann_json", "imgui-vr-helper-api", "devbench-api")

set_basename("FloatingDamageNG")

add_shflags("/DEBUG", { force = true })

set_configvar("VERSION_MAJOR", tonumber(ver[1]))
set_configvar("VERSION_MINOR", tonumber(ver[2]))
set_configvar("VERSION_PATCH", tonumber(ver[3]))
set_configvar("VERSION_STRING", version)

-- Numeric version defines available at compile time; the C++ side
-- stringifies them (avoids /D string-quoting headaches).
add_defines("FDNG_VERSION_MAJOR=" .. ver[1], "FDNG_VERSION_MINOR=" .. ver[2], "FDNG_VERSION_PATCH=" .. ver[3])

add_rules("commonlibsse-ng.plugin", {
    name = "FloatingDamageNG",
    author = "alandtse",
    description = "World-anchored floating damage numbers for Skyrim VR (and flat), via imgui-vr-helper world quads",
})

add_files("src/**.cpp|UI.cpp")
add_headerfiles("src/**.h")

add_includedirs("src")
set_pcxxheader("src/pch.h")

-- Auto-deploy on build. Looks at, in order:
--   1. SkyrimVRPluginTargets or SkyrimPluginTargets — semicolon-separated
--      list of Data folders (or mod-manager mod folders) for explicit
--      override. Both names are accepted since different setups on this
--      toolchain use either one.
--   2. SkyrimVRPath — community env var. Conventionally the SkyrimVR install
--      root (folder containing SkyrimVR.exe), but some setups point it at the
--      Data folder directly; we accept both.
-- If none are set, the deploy step is a no-op.
after_build(function(target)
    local function to_data_folder(p)
        p = p:trim()
        if os.isfile(path.join(p, "SkyrimVR.exe")) then
            return path.join(p, "Data")
        end
        return p
    end

    local function nonblank(v)
        if v and v:trim() ~= "" then
            return v
        end
        return nil
    end

    local function collect_targets()
        local explicit = nonblank(os.getenv("SkyrimVRPluginTargets")) or nonblank(os.getenv("SkyrimPluginTargets"))
        if explicit then
            local out = {}
            for _, dir in ipairs(explicit:split(";")) do
                dir = dir:trim()
                if dir ~= "" then
                    table.insert(out, dir)
                end
            end
            return out
        end
        local vr_root = os.getenv("SkyrimVRPath")
        if vr_root and vr_root ~= "" then
            return { to_data_folder(vr_root) }
        end
        return {}
    end

    local targets = collect_targets()
    if #targets == 0 then
        print("No deploy target (set SkyrimVRPath, SkyrimVRPluginTargets, or SkyrimPluginTargets)")
        return
    end

    local dll = target:targetfile()
    local pdb = target:symbolfile()
    -- Loose data the plugin loads at runtime (motion presets + drop-in folder
    -- READMEs) — the same tree the release archive ships, so in-game testing
    -- sees the shipped presets, not just the compiled Float. The INI is left
    -- alone (menu-managed) so a build never clobbers a tuned config.
    local dataDir = path.join(os.projectdir(), "Skyrim", "Data", "SKSE", "Plugins", "FloatingDamageNG")
    for _, dir in ipairs(targets) do
        local dest = path.join(dir, "SKSE", "Plugins")
        os.mkdir(dest)
        -- A running game holds the DLL/PDB locks; skip them with a warning
        -- instead of aborting, so the presets (never locked) still deploy and
        -- the other target still runs. The build itself already succeeded.
        try({
            function()
                os.cp(dll, dest)
                if os.isfile(pdb) then
                    os.cp(pdb, dest)
                end
                print("Deployed DLL to " .. dest)
            end,
            catch({
                function()
                    print("Skipped DLL for " .. dest .. " (game running?); presets still deploy")
                end,
            }),
        })
        if os.isdir(dataDir) then
            os.cp(dataDir, dest) -- -> <dest>/FloatingDamageNG/{Presets,Fonts}
            print("Deployed presets to " .. dest)
        end
    end
end)
