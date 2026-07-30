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

    const racket_smoke = b.addSystemCommand(&.{
        "racket", "tests/bootstrap-racket/run.rkt",
    });
    racket_smoke.step.dependOn(&install_l1i.step);
    racket_smoke.step.dependOn(&install_l1check.step);
    racket_smoke.step.dependOn(&install_l1bootstrap.step);
    const racket_smoke_step = b.step(
        "test-racket-bootstrap",
        "Translate Lain with Racket and execute the generated LAIN-IR",
    );
    racket_smoke_step.dependOn(&racket_smoke.step);

    const fixed_point = b.addSystemCommand(&.{
        "racket", "tests/bootstrap-racket/run-fixed-point.rkt",
    });
    fixed_point.step.dependOn(&install_l1i.step);
    fixed_point.step.dependOn(&install_l1check.step);
    fixed_point.step.dependOn(&install_l1bootstrap.step);
    fixed_point.step.dependOn(&install_lainc.step);
    const fixed_point_step = b.step(
        "bootstrap",
        "Build lainc and prove the pure-Lain stage2/stage3 fixed point",
    );
    fixed_point_step.dependOn(&fixed_point.step);

    const test_step = b.step(
        "test",
        "Run the standalone Racket/bootstrap tests",
    );
    test_step.dependOn(&racket_smoke.step);
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
