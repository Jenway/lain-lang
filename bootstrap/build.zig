const std = @import("std");

const c_flags = &.{
    "-std=gnu11",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-Wno-unused-variable",
};

const runtime_sources = &.{
    "src/lainir/lain_ir_parser.c",
    "src/lainir/lainir_core.c",
    "src/lainir/interpreter.c",
    "src/lainir/verifier.c",
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const l1i = addCExecutable(b, "l1i", target, optimize);
    l1i.root_module.addCSourceFiles(.{
        .files = &.{
            "src/lainir/lain_ir_interp_main.c",
            "src/lainir/lain_ir_parser.c",
            "src/lainir/lainir_core.c",
            "src/lainir/interpreter.c",
            "src/lainir/verifier.c",
        },
        .flags = c_flags,
    });
    const install_l1i = installNamed(
        b,
        l1i,
        "l1i",
        "Build the minimal LAIN-IR interpreter",
    );

    const l1check = addCExecutable(b, "l1check", target, optimize);
    l1check.root_module.addCSourceFiles(.{
        .files = &.{
            "src/lainir/lain_ir_check_main.c",
            "src/lainir/lain_ir_parser.c",
            "src/lainir/lainir_core.c",
            "src/lainir/emit_text.c",
            "src/lainir/verifier.c",
        },
        .flags = c_flags,
    });
    const install_l1check = installNamed(
        b,
        l1check,
        "l1check",
        "Build the minimal LAIN-IR parser and verifier",
    );

    const l1bootstrap = bootstrapExecutable(
        b,
        "l1bootstrap",
        target,
        optimize,
        false,
    );
    const install_l1bootstrap = installNamed(
        b,
        l1bootstrap,
        "l1bootstrap",
        "Build the generic frozen-compiler runner",
    );

    const lainc = bootstrapExecutable(b, "lainc", target, optimize, true);
    const install_lainc = installNamed(
        b,
        lainc,
        "lainc",
        "Build the driver for the pure-Lain compiler artifact",
    );
}

fn bootstrapExecutable(
    b: *std.Build,
    name: []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    driver: bool,
) *std.Build.Step.Compile {
    const executable = addCExecutable(b, name, target, optimize);
    executable.root_module.addCSourceFiles(.{
        .files = &.{"src/lainir/lain_ir_bootstrap_main.c"},
        .flags = c_flags,
    });
    executable.root_module.addCSourceFiles(.{
        .files = runtime_sources,
        .flags = c_flags,
    });
    if (driver)
        executable.root_module.addCMacro("LAIN_BOOTSTRAP_DRIVER", "1");
    return executable;
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
