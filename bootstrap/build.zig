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

    const lainir_print = addCExecutable(
        b,
        "lainir-print",
        "src/cli/print_main.c",
        target,
        optimize,
    );
    lainir_print.root_module.linkLibrary(core);
    lainir_print.root_module.linkLibrary(text);
    lainir_print.root_module.linkLibrary(interpreter);
    lainir_print.root_module.linkLibrary(host_io);
    _ = installNamed(
        b,
        lainir_print,
        "lainir-print",
        "Build the minimal LAIN-IR parser and verifier",
    );

    const lainir_seed = addCExecutable(
        b,
        "lainir-seed",
        "src/cli/seed_main.c",
        target,
        optimize,
    );
    linkBootstrap(
        lainir_seed,
        core,
        text,
        interpreter,
        host_io,
        bootstrap_host,
    );
    _ = installNamed(
        b,
        lainir_seed,
        "lainir-seed",
        "Run the LAIN-IR interpreter (interpreter|run subcommands)",
    );

    const lainir_lsp = addCExecutable(
        b,
        "lainir-lsp",
        "src/cli/lsp_main.c",
        target,
        optimize,
    );
    lainir_lsp.root_module.addCSourceFiles(.{
        .files = &.{"src/host/lsp_host.c"},
        .flags = c_flags,
    });
    lainir_lsp.root_module.linkLibrary(core);
    lainir_lsp.root_module.linkLibrary(text);
    lainir_lsp.root_module.linkLibrary(interpreter);
    lainir_lsp.root_module.linkLibrary(host_io);
    _ = installNamed(
        b,
        lainir_lsp,
        "lainir-lsp",
        "Run the LAIN-IR language server over stdio",
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
