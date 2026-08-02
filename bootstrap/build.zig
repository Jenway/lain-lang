const std = @import("std");

const c_flags = &.{
    "-std=gnu11",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-Wno-unused-variable",
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const core = addCLibrary(
        b,
        "lainir_core",
        &.{
            "src/core/lainir.c",
            "src/core/verifier.c",
        },
        target,
        optimize,
    );

    const text = addCLibrary(
        b,
        "lainir_text",
        &.{
            "src/text/parser.c",
            "src/text/emitter.c",
        },
        target,
        optimize,
    );
    text.root_module.linkLibrary(core);

    const interpreter = addCLibrary(
        b,
        "lainir_interpreter",
        &.{"src/interpreter/interpreter.c"},
        target,
        optimize,
    );
    interpreter.root_module.linkLibrary(core);

    const host_io = addCLibrary(
        b,
        "lainir_host_io",
        &.{"src/host/host_io.c"},
        target,
        optimize,
    );
    host_io.root_module.linkLibrary(core);

    const bootstrap_host = addCLibrary(
        b,
        "lainir_bootstrap_host",
        &.{"src/host/bootstrap.c"},
        target,
        optimize,
    );
    bootstrap_host.root_module.linkLibrary(core);
    bootstrap_host.root_module.linkLibrary(text);
    bootstrap_host.root_module.linkLibrary(interpreter);
    bootstrap_host.root_module.linkLibrary(host_io);

    const l1check = addCExecutable(
        b,
        "l1check",
        "src/cli/l1check.c",
        target,
        optimize,
    );
    l1check.root_module.linkLibrary(core);
    l1check.root_module.linkLibrary(text);
    l1check.root_module.linkLibrary(interpreter);
    l1check.root_module.linkLibrary(host_io);
    _ = installNamed(
        b,
        l1check,
        "l1check",
        "Build the minimal LAIN-IR parser and verifier",
    );

    const l1i = addCExecutable(
        b,
        "l1i",
        "src/cli/l1i.c",
        target,
        optimize,
    );
    l1i.root_module.linkLibrary(core);
    l1i.root_module.linkLibrary(text);
    l1i.root_module.linkLibrary(interpreter);
    l1i.root_module.linkLibrary(host_io);
    _ = installNamed(
        b,
        l1i,
        "l1i",
        "Build the minimal LAIN-IR interpreter",
    );

    const l1bootstrap = addCExecutable(
        b,
        "l1bootstrap",
        "src/cli/l1bootstrap.c",
        target,
        optimize,
    );
    linkBootstrap(
        l1bootstrap,
        core,
        text,
        interpreter,
        host_io,
        bootstrap_host,
    );
    _ = installNamed(
        b,
        l1bootstrap,
        "l1bootstrap",
        "Build the generic frozen-compiler runner",
    );

}

fn newCModule(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) *std.Build.Module {
    const module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    module.addIncludePath(b.path("include"));
    module.addIncludePath(b.path("src/host"));
    return module;
}

fn addCLibrary(
    b: *std.Build,
    name: []const u8,
    sources: []const []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) *std.Build.Step.Compile {
    const module = newCModule(b, target, optimize);
    module.addCSourceFiles(.{
        .files = sources,
        .flags = c_flags,
    });
    return b.addLibrary(.{
        .name = name,
        .root_module = module,
        .linkage = .static,
    });
}

fn addCExecutable(
    b: *std.Build,
    name: []const u8,
    source: []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) *std.Build.Step.Compile {
    const module = newCModule(b, target, optimize);
    module.addCSourceFiles(.{
        .files = &.{source},
        .flags = c_flags,
    });
    return b.addExecutable(.{
        .name = name,
        .root_module = module,
    });
}

fn linkBootstrap(
    executable: *std.Build.Step.Compile,
    core: *std.Build.Step.Compile,
    text: *std.Build.Step.Compile,
    interpreter: *std.Build.Step.Compile,
    host_io: *std.Build.Step.Compile,
    bootstrap_host: *std.Build.Step.Compile,
) void {
    executable.root_module.linkLibrary(core);
    executable.root_module.linkLibrary(text);
    executable.root_module.linkLibrary(interpreter);
    executable.root_module.linkLibrary(host_io);
    executable.root_module.linkLibrary(bootstrap_host);
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
