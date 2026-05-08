const std = @import("std");

pub fn build(b: *std.Build) void {
    const target      = b.standardTargetOptions(.{});
    const optimize    = b.standardOptimizeOption(.{});
    const native_game = b.option(bool, "native_game", "Route game logic through game.dll instead of VM") orelse true;

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
        "mathlib.c",
        "menu.c",
        "model.c",
        "net_dgrm.c", "net_loop.c", "net_main.c", "net_vcr.c",
        "nonintel.c",
        "pr_edict.c",
        "r_aclip.c", "r_alias.c", "r_bsp.c", "r_draw.c", "r_edge.c",
        "r_efrag.c", "r_light.c", "r_main.c", "r_misc.c", "r_part.c",
        "r_sky.c", "r_sprite.c", "r_surf.c", "r_vars.c",
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

    // QC VM interpreter and builtins — only needed for NATIVE_GAME=0
    const pr_vm_files: []const []const u8 = &.{
        "pr_cmds.c",
        "pr_exec.c",
    };

    const platform_files: []const []const u8 = &.{
        "sdlquake/platform/sys_sdl.c",
        "sdlquake/platform/sys_crash.c",
        "sdlquake/platform/vid_sdl.c",
        "sdlquake/platform/in_sdl.c",
        "sdlquake/platform/snd_sdl.c",
        "sdlquake/platform/net_sdl.c",
        "sdlquake/mcp/mcp_server.c",
        "sdlquake/engine/hotreload.c",
        "sdlquake/engine/sv_bridge.c",
        "sdlquake/engine/imgui_layer.c",
        "sdlquake/engine/r_debugdraw.c",
        "sdlquake/engine/r_bbox.c",
        "sdlquake/engine/r_paths.c",
        // Phase 7 in-game .map editor
        "sdlquake/engine/editor/editor.c",
        "sdlquake/engine/editor/edit_scene.c",
        "sdlquake/engine/editor/map_io.c",
        "sdlquake/engine/editor/brush_compile.c",
        "sdlquake/engine/editor/render_wire.c",
        "sdlquake/engine/editor/render_flat.c",
        "sdlquake/engine/editor/render_tex.c",
        "sdlquake/engine/editor/gizmo.c",
        "sdlquake/engine/editor/editor_ui.c",
        "sdlquake/engine/editor/collide.c",
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
    mod.addCMacro("NATIVE_GAME", if (native_game) "1" else "0");

    mod.addCSourceFiles(.{
        .root  = b.path(wq_dir),
        .files = engine_files,
        .flags = engine_c_flags,
    });
    if (!native_game) {
        mod.addCSourceFiles(.{
            .root  = b.path(wq_dir),
            .files = pr_vm_files,
            .flags = engine_c_flags,
        });
    }
    mod.addCSourceFiles(.{
        .files = platform_files,
        .flags = platform_c_flags,
    });

    // imgui Quake-state shim (includes quakedef.h, needs SDLQUAKE defined)
    mod.addCSourceFiles(.{
        .files = &.{"sdlquake/engine/imgui_support.c"},
        .flags = platform_c_flags,
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
    mod.addIncludePath(b.path(wq_dir));

    // ---------------------------------------------------------------------------
    // Vendored SDL3 (sdlquake/vendor/SDL3-3.4.8)
    // ---------------------------------------------------------------------------
    const sdl3_dir = "sdlquake/vendor/SDL3-3.4.8";
    mod.addIncludePath(b.path(sdl3_dir ++ "/include"));
    mod.addLibraryPath(b.path(sdl3_dir ++ "/lib/x64"));
    mod.linkSystemLibrary("SDL3", .{});
    mod.linkSystemLibrary("ws2_32", .{});  // Winsock for net_dgrm.c (inet_ntoa/inet_addr)
    mod.linkSystemLibrary("dbghelp", .{}); // SymInitialize / StackWalk64 for sys_crash.c

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
            "sdlquake/game/spawn.c",
            "sdlquake/game/subs.c",
            "sdlquake/game/combat.c",
            "sdlquake/game/world.c",
            "sdlquake/game/client.c",
            "sdlquake/game/player.c",
            "sdlquake/game/items.c",
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

    // Install game.dll alongside quake.exe (part of the default build).
    const game_install = b.addInstallArtifact(game_lib, .{});
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
    b.installArtifact(exe);

    // Install SDL3.dll next to the executable so it can be found at runtime.
    b.installBinFile(sdl3_dir ++ "/lib/x64/SDL3.dll", "SDL3.dll");

    // ---------------------------------------------------------------------------
    // Run step: zig build run -- [quake args]
    // ---------------------------------------------------------------------------
    const run = b.addRunArtifact(exe);
    run.step.dependOn(b.getInstallStep());
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
    const extract_run = b.addRunArtifact(extract_exe);
    extract_run.setCwd(b.path(""));
    if (b.args) |args| extract_run.addArgs(args);
    b.step("extract", "Extract Wolf3D + Doom1 weapon assets into id1/").dependOn(&extract_run.step);
}
