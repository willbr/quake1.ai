const std = @import("std");

pub fn build(b: *std.Build) void {
    const target   = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

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

    const wq_dir = "Quake-master/WinQuake";

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
        "pr_cmds.c", "pr_edict.c", "pr_exec.c",
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

    const platform_files: []const []const u8 = &.{
        "sdlquake/platform/sys_sdl.c",
        "sdlquake/platform/vid_sdl.c",
        "sdlquake/platform/in_sdl.c",
        "sdlquake/platform/snd_sdl.c",
        "sdlquake/platform/net_sdl.c",
        "sdlquake/mcp/mcp_server.c",
    };

    // ---------------------------------------------------------------------------
    // Root module (Zig 0.16: C files / includes / libs live on the Module)
    // ---------------------------------------------------------------------------
    const mod = b.createModule(.{
        .target    = target,
        .optimize  = optimize,
        .link_libc = true,
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

    // Platform stubs come first so our winquake.h / mgraph.h shadow the originals.
    mod.addIncludePath(b.path("sdlquake/platform"));
    mod.addIncludePath(b.path("sdlquake/mcp"));
    mod.addIncludePath(b.path(wq_dir));

    // ---------------------------------------------------------------------------
    // Vendored SDL3 (sdlquake/vendor/SDL3-3.4.8)
    // ---------------------------------------------------------------------------
    const sdl3_dir = "sdlquake/vendor/SDL3-3.4.8";
    mod.addIncludePath(b.path(sdl3_dir ++ "/include"));
    mod.addLibraryPath(b.path(sdl3_dir ++ "/lib/x64"));
    mod.linkSystemLibrary("SDL3", .{});
    mod.linkSystemLibrary("ws2_32", .{});  // Winsock for net_dgrm.c (inet_ntoa/inet_addr)

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
}
