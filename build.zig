const std = @import("std");

pub fn build(b: *std.Build) void {
    const target      = b.standardTargetOptions(.{});
    const optimize    = b.standardOptimizeOption(.{});

    const os_tag = target.result.os.tag;
    const is_windows = os_tag == .windows;
    const is_macos   = os_tag == .macos;

    // Engine files are K&R/C89 era; gnu89 + fcommon matches original MSVC tentative-definition behaviour.
    const engine_c_flags: []const []const u8 = &.{
        "-DSDLQUAKE",
        "-fno-strict-aliasing",
        "-fwrapv",
        "-std=gnu89",
        "-fcommon",
        "-w",
        // Original engine code intentionally relies on float->int truncation and
        // other patterns that are UB by the C standard but well-defined on x86.
        // Suppress UBSan so these don't panic at runtime.
        "-fno-sanitize=undefined",
    };
    const platform_c_flags: []const []const u8 = &.{
        "-DSDLQUAKE",
        "-fno-strict-aliasing",
        "-fwrapv",
        "-w",
    };

    const wq_dir = "sdlquake/engine_src";

    const engine_files: []const []const u8 = &.{
        "chase.c",
        "cl_demo.c", "cl_input.c", "cl_main.c", "cl_parse.c", "cl_tent.c",
        "cmd.c",
        "common.c",
        "console.c",
        "crc.c",
        "cvar.c",
        "draw.c",
        "d_edge.c", "d_fill.c", "d_init.c", "d_modech.c", "d_part.c",
        "d_polyse.c", "d_scan.c", "d_sky.c", "d_sprite.c", "d_surf.c",
        "d_vars.c", "d_zpoint.c",
        "host.c", "host_cmd.c",
        "keys.c",
        "line_editor.c",
        "mathlib.c",
        "menu.c",
        "model.c",
        "net_dgrm.c", "net_loop.c", "net_main.c", "net_vcr.c",
        "nonintel.c",
        "pr_edict.c",
        "r_aclip.c", "r_alias.c", "r_bsp.c", "r_draw.c", "r_drawflat.c", "r_edge.c",
        "r_efrag.c", "r_light.c", "r_livelight.c", "r_lut.c", "r_main.c", "r_misc.c", "r_part.c",
        "r_decals.c",
        "r_sky.c", "r_sprite.c", "r_surf.c", "r_surf_rgb.c", "r_vars.c",
        "r_fog.c",
        "sbar.c",
        "screen.c",
        "snd_dma.c", "snd_mem.c", "snd_mix.c",
        "sv_main.c", "sv_move.c", "sv_phys.c", "sv_user.c",
        "view.c",
        "wad.c",
        "world.c",
        "zone.c",
        "cd_null.c",
    };

    const platform_files: []const []const u8 = &.{
        "sdlquake/platform/sys_sdl.c",
        "sdlquake/platform/sys_crash.c",
        "sdlquake/platform/vid_sdl.c",
        "sdlquake/platform/in_sdl.c",
        "sdlquake/platform/snd_sdl.c",
        "sdlquake/platform/net_sdl.c",
        "sdlquake/platform/screenshot_path.c",
        "sdlquake/platform/clipboard.c",
        "sdlquake/platform/crop_screenshot.c",
        "sdlquake/mcp/mcp_server.c",
        "sdlquake/engine/hotreload.c",
        "sdlquake/engine/sv_bridge.c",
        "sdlquake/engine/imgui_layer.c",
        "sdlquake/engine/imgui_debug_panel.c",
        "sdlquake/engine/r_debugdraw.c",
        "sdlquake/engine/debug_lines.c",
        "sdlquake/engine/r_bbox.c",
        "sdlquake/engine/r_paths.c",
        "sdlquake/engine/virtual_fs.c",
        "sdlquake/engine/perf.c",
        // Phase 7 in-game .map editor
        "sdlquake/engine/editor/editor.c",
        "sdlquake/engine/editor/edit_scene.c",
        "sdlquake/engine/editor/edit_history.c",
        "sdlquake/engine/editor/map_io.c",
        "sdlquake/engine/editor/brush_compile.c",
        "sdlquake/engine/editor/render_wire.c",
        "sdlquake/engine/editor/render_flat.c",
        "sdlquake/engine/editor/render_tex.c",
        "sdlquake/engine/editor/gizmo.c",
        "sdlquake/engine/editor/editor_ui.c",
        "sdlquake/engine/editor/edit_texcache.c",
        "sdlquake/engine/editor/editor_classlist.c",
        "sdlquake/engine/editor/collide.c",
        "sdlquake/engine/editor/light_bake_thread.c",
    };

    const imgui_dir = "sdlquake/vendor/imgui-1.92.8";
    const imgui_cpp_flags: []const []const u8 = &.{
        "-std=c++17",
        "-fno-strict-aliasing",
        "-w",
    };

    // ---------------------------------------------------------------------------
    // Root module (Zig 0.16: C files / includes / libs live on the Module)
    // ---------------------------------------------------------------------------
    const mod = b.createModule(.{
        .target      = target,
        .optimize    = optimize,
        .link_libc   = true,
        .link_libcpp = true,
    });
    mod.addCSourceFiles(.{
        .root  = b.path(wq_dir),
        .files = engine_files,
        .flags = engine_c_flags,
    });
    mod.addCSourceFiles(.{
        .files = platform_files,
        .flags = platform_c_flags,
    });

    // imgui Quake-state shim (includes quakedef.h, needs SDLQUAKE defined)
    mod.addCSourceFiles(.{
        .files = &.{"sdlquake/engine/imgui_support.c"},
        .flags = platform_c_flags,
    });

    // Vendored qbsp (id Software, GPLv2). Compiles in-process so the editor
    // can recompile the .map and load the resulting .bsp without leaving the
    // engine. Same C dialect quirks as the engine source — gnu89 + fcommon +
    // no UB sanitizer. Suppresses the firehose of warnings 30-year-old C
    // emits with -w.
    // qbsp/light/vis are 1996-era utils that key #include <direct.h> off WIN32.
    // POSIX builds use <unistd.h> via the cmdlib code paths already present.
    const qbsp_c_flags: []const []const u8 = if (is_windows) &.{
        "-DWIN32",                 // gates the cmdlib.c <direct.h> path
        "-DDOUBLEVEC_T",
        "-include", "sdlquake/vendor/qbsp/qbsp_namespace.h",
        "-fno-strict-aliasing",
        "-fwrapv",
        "-std=gnu89",
        "-fcommon",
        "-w",
        "-fno-sanitize=undefined",
    } else &.{
        "-DDOUBLEVEC_T",
        "-include", "sdlquake/vendor/qbsp/qbsp_namespace.h",
        "-fno-strict-aliasing",
        "-fwrapv",
        "-std=gnu89",
        "-fcommon",
        "-w",
        "-fno-sanitize=undefined",
    };
    // File set lifted directly from qbsp's MAKEFILE QBSPFILES = ... list:
    // qbsp-specific (brush, csg4, ...) plus three COMMON files (cmdlib,
    // mathlib, bspfile). polylib/scriplib/wadlib were for light/vis/
    // texmake — including them duplicates winding helpers and breaks the
    // link.
    mod.addCSourceFiles(.{
        .files = &.{
            "sdlquake/vendor/qbsp/brush.c",
            "sdlquake/vendor/qbsp/bspfile.c",
            "sdlquake/vendor/qbsp/cmdlib.c",
            "sdlquake/vendor/qbsp/csg4.c",
            "sdlquake/vendor/qbsp/map.c",
            "sdlquake/vendor/qbsp/mathlib.c",
            "sdlquake/vendor/qbsp/merge.c",
            "sdlquake/vendor/qbsp/nodraw.c",
            "sdlquake/vendor/qbsp/outside.c",
            "sdlquake/vendor/qbsp/portals.c",
            "sdlquake/vendor/qbsp/qbsp.c",
            "sdlquake/vendor/qbsp/region.c",
            "sdlquake/vendor/qbsp/solidbsp.c",
            "sdlquake/vendor/qbsp/surfaces.c",
            "sdlquake/vendor/qbsp/tjunc.c",
            "sdlquake/vendor/qbsp/writebsp.c",
            "sdlquake/vendor/qbsp/qbsp_lib.c",
        },
        .flags = qbsp_c_flags,
    });
    // Intentionally NOT on the global include path — qbsp's mathlib.h and
    // cmdlib.h would collide with the engine's. qbsp's own .c files use
    // `#include "..."` which clang resolves relative to the source file's
    // directory automatically.

    // Vendored id-LIGHT (id Software, GPLv2). Same in-process pattern as
    // qbsp: the editor runs qbsp_compile_to_memory(), then
    // light_compile_to_memory() to bake lighting on the BSP qbsp left
    // sitting in shared globals (dfaces / dplanes / dlightdata / ...).
    // Light's own .c files include cmdlib.h/mathlib.h/bspfile.h which
    // live in vendor/qbsp/ — pass -Isdlquake/vendor/qbsp via the flags
    // so clang resolves those, then -include both namespace headers so
    // qbsp's symbol renames apply (cmdlib/mathlib/bspfile) on top of
    // light's own renames (entities, LightFace, ...).
    // light's .c files include "cmdlib.h" / "mathlib.h" / "bspfile.h" --
    // those headers live alongside the source in vendor/light/ (verbatim
    // copies of vendor/qbsp/ originals) so clang's "directory of source"
    // search resolves them locally. The .c implementations live in
    // vendor/qbsp/ and are linked into the binary once; light's TUs
    // reference them via the matching header declarations.
    const light_c_flags: []const []const u8 = if (is_windows) &.{
        "-DWIN32",
        "-DDOUBLEVEC_T",
        "-include", "sdlquake/vendor/qbsp/qbsp_namespace.h",
        "-include", "sdlquake/vendor/light/light_namespace.h",
        "-fno-strict-aliasing",
        "-fwrapv",
        "-std=gnu89",
        "-fcommon",
        "-w",
        "-fno-sanitize=undefined",
    } else &.{
        "-DDOUBLEVEC_T",
        "-include", "sdlquake/vendor/qbsp/qbsp_namespace.h",
        "-include", "sdlquake/vendor/light/light_namespace.h",
        "-fno-strict-aliasing",
        "-fwrapv",
        "-std=gnu89",
        "-fcommon",
        "-w",
        "-fno-sanitize=undefined",
    };
    mod.addCSourceFiles(.{
        .files = &.{
            "sdlquake/vendor/light/light.c",
            "sdlquake/vendor/light/entities.c",
            "sdlquake/vendor/light/ltface.c",
            "sdlquake/vendor/light/trace.c",
            "sdlquake/vendor/light/light_lib.c",
        },
        .flags = light_c_flags,
    });

    // Vendored id-VIS (id Software, GPLv2). Third member of the in-process
    // compile trio. Caller invokes qbsp_compile_to_memory first (which
    // leaves dfaces / dplanes / dleafs / etc. populated and writes a .prt
    // alongside the destname), then vis_compile_in_place to fill dvisdata
    // from that .prt, then light_compile_to_memory to bake lighting and
    // re-serialise the final BSP. VIS's same-named types and globals
    // (winding_t / plane_t / portal_t / leaf_t / numportals / portals / ...)
    // collide with qbsp's; vis_namespace.h prefixes them.
    const vis_c_flags: []const []const u8 = if (is_windows) &.{
        "-DWIN32",
        "-DDOUBLEVEC_T",
        "-include", "sdlquake/vendor/qbsp/qbsp_namespace.h",
        "-include", "sdlquake/vendor/vis/vis_namespace.h",
        "-fno-strict-aliasing",
        "-fwrapv",
        "-std=gnu89",
        "-fcommon",
        "-w",
        "-fno-sanitize=undefined",
    } else &.{
        "-DDOUBLEVEC_T",
        "-include", "sdlquake/vendor/qbsp/qbsp_namespace.h",
        "-include", "sdlquake/vendor/vis/vis_namespace.h",
        "-fno-strict-aliasing",
        "-fwrapv",
        "-std=gnu89",
        "-fcommon",
        "-w",
        "-fno-sanitize=undefined",
    };
    mod.addCSourceFiles(.{
        .files = &.{
            "sdlquake/vendor/vis/vis.c",
            "sdlquake/vendor/vis/flow.c",
            "sdlquake/vendor/vis/soundpvs.c",
            "sdlquake/vendor/vis/vis_lib.c",
        },
        .flags = vis_c_flags,
    });

    // Dear ImGui core + SDL3/SDL_Renderer backends + our C++ bridge (no logic)
    mod.addCSourceFiles(.{
        .files = &.{
            imgui_dir ++ "/imgui.cpp",
            imgui_dir ++ "/imgui_draw.cpp",
            imgui_dir ++ "/imgui_tables.cpp",
            imgui_dir ++ "/imgui_widgets.cpp",
            imgui_dir ++ "/backends/imgui_impl_sdl3.cpp",
            imgui_dir ++ "/backends/imgui_impl_sdlrenderer3.cpp",
            "sdlquake/engine/imgui_bridge.cpp",
        },
        .flags = imgui_cpp_flags,
    });
    mod.addIncludePath(b.path(imgui_dir));

    // Platform stubs come first so our winquake.h / mgraph.h shadow the originals.
    mod.addIncludePath(b.path("sdlquake/platform"));
    mod.addIncludePath(b.path("sdlquake/mcp"));
    mod.addIncludePath(b.path("sdlquake/engine"));
    mod.addIncludePath(b.path("sdlquake/engine/editor"));
    mod.addIncludePath(b.path("sdlquake/game"));
    mod.addIncludePath(b.path("sdlquake/vendor/stb"));
    mod.addIncludePath(b.path(wq_dir));

    // ---------------------------------------------------------------------------
    // Vendored SDL3 (sdlquake/vendor/SDL3-3.4.8)
    // Per-OS lib dir: lib/x64 (Windows .lib + .dll), lib/macos (.dylib).
    // ---------------------------------------------------------------------------
    const sdl3_dir = "sdlquake/vendor/SDL3-3.4.8";
    mod.addIncludePath(b.path(sdl3_dir ++ "/include"));
    if (is_windows) {
        mod.addLibraryPath(b.path(sdl3_dir ++ "/lib/x64"));
        mod.linkSystemLibrary("SDL3", .{});
        mod.linkSystemLibrary("ws2_32", .{});   // Winsock for net_dgrm.c
        mod.linkSystemLibrary("dbghelp", .{});  // dbghelp for sys_crash.c
    } else if (is_macos) {
        mod.addLibraryPath(b.path(sdl3_dir ++ "/lib/macos"));
        // search_strategy = .no_fallback so zig doesn't also probe Homebrew
        // (/opt/homebrew/lib) and silently pick the system SDL3 install,
        // baking that absolute path into the binary's LC_LOAD_DYLIB.
        // The vendored libSDL3.0.dylib has its install_name set to
        // @rpath/libSDL3.0.dylib; the executable adds @executable_path
        // as an rpath below so the dylib is found next to the binary.
        mod.linkSystemLibrary("SDL3", .{ .search_strategy = .no_fallback });
    } else {
        // Linux / other POSIX: rely on system SDL3 (pkg-config style).
        mod.linkSystemLibrary("SDL3", .{});
    }

    // ---------------------------------------------------------------------------
    // Game DLL (hot-reloadable)
    // ---------------------------------------------------------------------------
    const game_mod = b.createModule(.{
        .target    = target,
        .optimize  = optimize,
        .link_libc = true,
    });
    game_mod.addIncludePath(b.path("sdlquake/game"));
    game_mod.addIncludePath(b.path("sdlquake/engine"));
    game_mod.addCSourceFiles(.{
        .files = &.{
            "sdlquake/game/game_main.c",
            "sdlquake/game/abilities.c",
            "sdlquake/game/spawn.c",
            "sdlquake/game/subs.c",
            "sdlquake/game/combat.c",
            "sdlquake/game/world.c",
            "sdlquake/game/client.c",
            "sdlquake/game/player.c",
            "sdlquake/game/items.c",
            "sdlquake/game/items_push.c",
            "sdlquake/game/weapons.c",
            "sdlquake/game/weapons_phase6.c",
            "sdlquake/game/player_phase6.c",
            "sdlquake/game/fight.c",
            "sdlquake/game/ai.c",
            "sdlquake/game/misc.c",
            "sdlquake/game/doors.c",
            "sdlquake/game/buttons.c",
            "sdlquake/game/triggers.c",
            "sdlquake/game/plats.c",
            "sdlquake/game/monsters.c",
            "sdlquake/game/monster_fish.c",
            "sdlquake/game/monster_tarbaby.c",
            "sdlquake/game/monster_soldier.c",
            "sdlquake/game/monster_dog.c",
            "sdlquake/game/monster_enforcer.c",
            "sdlquake/game/monster_knight.c",
            "sdlquake/game/monster_demon.c",
            "sdlquake/game/monster_zombie.c",
            "sdlquake/game/monster_ogre.c",
            "sdlquake/game/monster_wizard.c",
            "sdlquake/game/monster_hknight.c",
            "sdlquake/game/monster_shalrath.c",
            "sdlquake/game/monster_shambler.c",
            "sdlquake/game/monster_boss.c",
            "sdlquake/game/monster_oldone.c",
            // Immersive-sim systems (M1 + M2 + M2.5)
            "sdlquake/game/sim/sim_main.c",
            "sdlquake/game/sim/sim_stimulus.c",
            "sdlquake/game/sim/sim_ai.c",
            "sdlquake/game/sim/sim_nav.c",
            "sdlquake/game/sim/sim_arena.c",
            "sdlquake/game/sim/sim_wind.c",
            "sdlquake/game/sim/sim_light.c",
            "sdlquake/game/sim/sim_retrofit.c",
        },
        .flags = &.{
            "-std=c11",
            "-fno-strict-aliasing",
            "-w",
            // Game DLL was ported from QuakeC, which itself uses float→int
            // casts that are UB by the standard but well-defined on x86. Same
            // rationale as the engine (see engine_c_flags above).
            "-fno-sanitize=undefined",
        },
    });
    const game_lib = b.addLibrary(.{
        .name        = "game",
        .root_module = game_mod,
        .linkage     = .dynamic,
    });

    // Install the game shared library alongside the engine binary.
    // On Windows zig writes `game.dll` to bin/. On macOS the default would be
    // `libgame.dylib` under lib/ -- but hotreload.c expects `game.<ext>` in
    // bin/, so we install it explicitly there with the bare name.
    const game_install = if (is_windows)
        b.addInstallArtifact(game_lib, .{})
    else blk: {
        const game_ext: []const u8 = if (is_macos) ".dylib" else ".so";
        break :blk b.addInstallArtifact(game_lib, .{
            .dest_dir    = .{ .override = .bin },
            .dest_sub_path = b.fmt("game{s}", .{game_ext}),
        });
    };
    b.getInstallStep().dependOn(&game_install.step);

    // `zig build game` rebuilds only the game DLL — fast hot-reload iteration.
    const game_step = b.step("game", "Rebuild game.dll only (hot-reload iteration)");
    game_step.dependOn(&game_install.step);

    // ---------------------------------------------------------------------------
    // Executable
    // ---------------------------------------------------------------------------
    const exe = b.addExecutable(.{
        .name        = "quake",
        .root_module = mod,
    });
    if (is_macos) {
        // Find the vendored libSDL3.0.dylib via the binary's own folder.
        exe.root_module.addRPath(.{ .cwd_relative = "@executable_path" });
    }
    const exe_install = b.addInstallArtifact(exe, .{});
    b.getInstallStep().dependOn(&exe_install.step);

    // Install the SDL3 shared library next to the executable.
    if (is_windows) {
        b.installBinFile(sdl3_dir ++ "/lib/x64/SDL3.dll", "SDL3.dll");
    } else if (is_macos) {
        b.installBinFile(sdl3_dir ++ "/lib/macos/libSDL3.0.dylib", "libSDL3.0.dylib");

        // Zig's macOS linking unconditionally adds /opt/homebrew/lib to the
        // dyld search path, so the resulting binary records an absolute
        // dependency on whichever libSDL3.0.dylib Homebrew has installed
        // (or fails entirely if none is present). Rewrite that LC_LOAD_DYLIB
        // entry to the @rpath-relative form that our vendored dylib uses, so
        // the binary resolves the dylib alongside itself.
        const fixup = b.addSystemCommand(&.{
            "install_name_tool", "-change",
            "/opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib",
            "@rpath/libSDL3.0.dylib",
            b.getInstallPath(.bin, "quake"),
        });
        fixup.step.dependOn(&exe_install.step);
        b.getInstallStep().dependOn(&fixup.step);
    }

    // ---------------------------------------------------------------------------
    // Run step: zig build run -- [quake args]
    // ---------------------------------------------------------------------------
    const run = b.addRunArtifact(exe);
    run.step.dependOn(b.getInstallStep());
    // Pin cwd to the project root so id1/ resolves regardless of which
    // subdir the user invoked zig from. The engine's basedir comes from
    // SDL_GetCurrentDirectory() at startup.
    run.setCwd(b.path(""));
    if (b.args) |args| run.addArgs(args);
    b.step("run", "Build and run Quake").dependOn(&run.step);

    // ---------------------------------------------------------------------------
    // Phase 6 asset extractor: zig build extract
    //   Reads ref/wolf3d-data/ and ref/doom-data/, writes loose .spr/.wav into id1/.
    // ---------------------------------------------------------------------------
    const extract_mod = b.createModule(.{
        .root_source_file = b.path("tools/extract_phase6/extract.zig"),
        .target           = b.graph.host,
        .optimize         = optimize,
    });
    const extract_exe = b.addExecutable(.{
        .name        = "extract_phase6",
        .root_module = extract_mod,
    });
    // User-facing `zig build extract [-- args]` — preserves the dump/test
    // sub-modes (-test_palette etc.).
    const extract_run = b.addRunArtifact(extract_exe);
    extract_run.setCwd(b.path(""));
    if (b.args) |args| extract_run.addArgs(args);
    b.step("extract", "Extract Wolf3D + Doom1 weapon assets into id1/").dependOn(&extract_run.step);

    // Auto-extract on every `zig build` — the extractor stat-skips when all
    // outputs are present (see manifest.allOutputsExist), so the steady-state
    // cost is one process launch. Inputs (DOOM1.WAD, VSWAP.WL1, pak0.pak)
    // are committed reference data; `rm id1/progs/v_doom*.spr` to force a
    // re-extract. No args passed here so the no-args extract-everything path
    // is taken regardless of what's after `--` on the build command.
    const extract_auto = b.addRunArtifact(extract_exe);
    extract_auto.setCwd(b.path(""));
    b.getInstallStep().dependOn(&extract_auto.step);
}
