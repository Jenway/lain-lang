const std = @import("std");

const common_c_flags = &.{
    "-std=gnu11",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-Wno-unused-variable",
};

const lainc_sources = &.{
    "src/compiler/native_runtime.c",
    "src/compiler/lainir_exec.c",
    "src/compiler/native_compiler.c",
    "src/compiler/builder_ffi.c",
    "src/compiler/structured_unit.c",
    "src/lainir/lainir_core.c",
    "src/lainir/lain_ir_parser.c",
    "src/lainir/verifier.c",
    "src/lainast/lain_ast.c",
    "src/lainast/lain_ast_parser.c",
    "src/lainir/emitter.c",
    "src/lainir/emit_text.c",
    "src/lainir/interpreter.c",
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const scheme = b.option(
        []const u8,
        "scheme",
        "Scheme backend for lainc: gauche or chibi (default: gauche on Windows, chibi elsewhere)",
    ) orelse if (target.result.os.tag == .windows) "gauche" else "chibi";

    const lainc = addCExecutable(b, "lainc", target, optimize);
    lainc.root_module.addCSourceFiles(.{ .files = lainc_sources, .flags = common_c_flags });
    // Build orchestration belongs to Meta/Lain. Disabling the legacy C source
    // scanner also keeps the stage-0 compiler portable across GCC and Clang.
    lainc.root_module.addCMacro("LAIN_DISABLE_NATIVE_BUILD", "1");
    if (std.mem.eql(u8, scheme, "gauche")) {
        lainc.root_module.addCSourceFiles(.{
            .files = &.{"src/compiler/vm_gauche.c"},
            .flags = common_c_flags,
        });
        lainc.root_module.addCMacro("LAIN_SCHEME_BACKEND_GAUCHE", "1");
        addGaucheConfiguration(b, lainc.root_module, target.result.os.tag == .windows);
    } else if (std.mem.eql(u8, scheme, "chibi")) {
        lainc.root_module.addCSourceFiles(.{
            .files = &.{"src/compiler/vm_chibi.c"},
            .flags = common_c_flags,
        });
        lainc.root_module.addCMacro("LAIN_SCHEME_BACKEND_CHIBI", "1");
        lainc.root_module.addIncludePath(b.path("third_party/chibi-scheme/include"));
        lainc.root_module.addLibraryPath(b.path("third_party/chibi-scheme"));
        lainc.root_module.linkSystemLibrary("chibi-scheme", .{ .use_pkg_config = .no });
        if (target.result.os.tag != .windows)
            lainc.root_module.linkSystemLibrary("dl", .{});
    } else {
        std.debug.panic("unknown -Dscheme={s}; expected gauche or chibi", .{scheme});
    }
    const install_lainc = installNamed(
        b, lainc, "lainc", "Build and install the stage-0 Lain host");

    const l1c = addCExecutable(b, "l1c", target, optimize);
    l1c.root_module.addCSourceFiles(.{
        .files = &.{
            "src/lainir/lain_ir_main.c",
            "src/lainir/lain_ir_parser.c",
            "src/lainir/lainir_core.c",
            "src/lainir/emitter.c",
            "src/lainir/emit_text.c",
        },
        .flags = common_c_flags,
    });
    _ = installNamed(b, l1c, "l1c", "Build and install the LAIN-IR C emitter");

    const l1i = addCExecutable(b, "l1i", target, optimize);
    l1i.root_module.addCSourceFiles(.{
        .files = &.{
            "src/lainir/lain_ir_interp_main.c",
            "src/lainir/lain_ir_parser.c",
            "src/lainir/lainir_core.c",
            "src/lainir/interpreter.c",
            "src/lainir/verifier.c",
        },
        .flags = common_c_flags,
    });
    _ = installNamed(b, l1i, "l1i", "Build and install the LAIN-IR interpreter");

    const l1check = addCExecutable(b, "l1check", target, optimize);
    l1check.root_module.addCSourceFiles(.{
        .files = &.{
            "src/lainir/lain_ir_check_main.c",
            "src/lainir/lain_ir_parser.c",
            "src/lainir/lainir_core.c",
            "src/lainir/emit_text.c",
            "src/lainir/verifier.c",
        },
        .flags = common_c_flags,
    });
    const install_l1check = installNamed(
        b, l1check, "l1check", "Parse, verify, and canonicalize LAIN-IR");

    const generate_self_hosted = b.addSystemCommand(&.{
        "python", "tests/core/self_hosting/build_self_hosted_compiler.py",
    });
    generate_self_hosted.step.dependOn(&install_lainc.step);
    generate_self_hosted.step.dependOn(&install_l1check.step);
    const install_self_hosted = b.addInstallFileWithDir(
        .{ .cwd_relative = "build/core-self-hosting/stage2_compiler.l1" },
        .bin,
        "stage2_compiler.l1",
    );
    install_self_hosted.step.dependOn(&generate_self_hosted.step);
    b.getInstallStep().dependOn(&install_self_hosted.step);
    const self_hosted_step = b.step(
        "self-host-compiler",
        "Build and install the stage-2 self-hosted Lain compiler artifact",
    );
    self_hosted_step.dependOn(&install_self_hosted.step);

    const test_step = b.step("test", "Build all tools and run the core test suite");
    const run_tests = b.addSystemCommand(&.{ "python", "tests/core_runner.py" });
    run_tests.step.dependOn(b.getInstallStep());
    test_step.dependOn(&run_tests.step);

    const self_host_step = b.step(
        "test-self-host",
        "Run the generated structured L1 execution closure",
    );
    const run_self_host = b.addSystemCommand(&.{
        "python", "tests/core/self_hosting/run_self_hosting.py",
    });
    run_self_host.step.dependOn(b.getInstallStep());
    self_host_step.dependOn(&run_self_host.step);
}

fn addCExecutable(
    b: *std.Build,
    name: []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) *std.Build.Step.Compile {
    const module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    module.addIncludePath(b.path("src"));
    return b.addExecutable(.{ .name = name, .root_module = module });
}

fn installNamed(
    b: *std.Build,
    artifact: *std.Build.Step.Compile,
    name: []const u8,
    description: []const u8,
) *std.Build.Step.InstallArtifact {
    const install = b.addInstallArtifact(artifact, .{});
    b.getInstallStep().dependOn(&install.step);
    const step = b.step(name, description);
    step.dependOn(&install.step);
    return install;
}

fn addGaucheConfiguration(b: *std.Build, module: *std.Build.Module, windows: bool) void {
    addConfigFlags(b, module, b.run(&.{ "gauche-config", "-I" }), false);
    const library_output = b.run(&.{ "gauche-config", "-L" });
    addConfigFlags(b, module, library_output, false);
    if (windows) {
        var words = std.mem.tokenizeAny(u8, library_output, " \t\r\n");
        while (words.next()) |word| {
            if (std.mem.startsWith(u8, word, "-L")) {
                const lib_dir = word[2..];
                module.addObjectFile(.{ .cwd_relative = b.pathJoin(&.{ lib_dir, "libgauche-0.98.dll" }) });
                const gauche_root = ancestor(lib_dir, 4);
                installRuntimeDll(b, b.pathJoin(&.{ gauche_root, "bin", "libgauche-0.98.dll" }));
                installRuntimeDll(b, b.pathJoin(&.{ gauche_root, "bin", "libwinpthread-1.dll" }));
                break;
            }
        }
    }
    addConfigFlags(b, module, b.run(&.{ "gauche-config", "-l" }), windows);
}

fn ancestor(path: []const u8, count: usize) []const u8 {
    var current = path;
    for (0..count) |_| current = std.fs.path.dirname(current) orelse
        std.debug.panic("cannot find Gauche root from {s}", .{path});
    return current;
}

fn installRuntimeDll(b: *std.Build, path: []const u8) void {
    const install = b.addInstallFileWithDir(
        .{ .cwd_relative = b.dupe(path) },
        .bin,
        std.fs.path.basename(path),
    );
    b.getInstallStep().dependOn(&install.step);
}

fn addConfigFlags(
    b: *std.Build,
    module: *std.Build.Module,
    output: []const u8,
    skip_windows_gauche: bool,
) void {
    var words = std.mem.tokenizeAny(u8, output, " \t\r\n");
    while (words.next()) |word| {
        if (std.mem.startsWith(u8, word, "-I")) {
            module.addSystemIncludePath(.{ .cwd_relative = b.dupe(word[2..]) });
        } else if (std.mem.startsWith(u8, word, "-L")) {
            module.addLibraryPath(.{ .cwd_relative = b.dupe(word[2..]) });
        } else if (std.mem.startsWith(u8, word, "-l")) {
            if (skip_windows_gauche and std.mem.eql(u8, word[2..], "gauche-0.98"))
                continue;
            module.linkSystemLibrary(b.dupe(word[2..]), .{ .use_pkg_config = .no });
        }
    }
}
