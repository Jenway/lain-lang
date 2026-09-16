const std = @import("std");

const c_flags = &.{
    "-std=gnu11",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-Wno-unused-variable",
};

// The native host drivers are compiled by the Python build scripts with plain
// `zig cc`, so the Zig targets covering them mirror those command lines
// instead of reusing the seed interpreter's warning flags.
const native_c_flags = &.{"-std=c11"};
const native_lainc_c_flags = &.{ "-std=c11", "-DLAIN_NATIVE_LIBRARY_ENTRY" };

// Each driver has its own link closure, and the difference is load-bearing:
// interpreter/eval_source.c calls lainir_emit_text_module, so the seed
// compiler closure needs text/emitter.c; native_lainc.c's --run path calls
// lainir_host_read_file, so the lainc closure needs host/host_io.c.  These
// mirror SEED_C_SOURCES and IN_PROCESS_SOURCES in the matching scripts.
const native_compiler_sources = &.{
    "src/core/lainir.c",
    "src/core/verifier.c",
    "src/text/parser.c",
    "src/text/emitter.c",
    "src/interpreter/interpreter.c",
    "src/interpreter/eval_source.c",
    "src/interpreter/vm_control.c",
};
const native_lainc_sources = &.{
    "src/core/lainir.c",
    "src/core/verifier.c",
    "src/text/parser.c",
    "src/interpreter/interpreter.c",
    "src/interpreter/vm_control.c",
    "src/host/host_io.c",
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
        &.{
            "src/interpreter/interpreter.c",
            "src/interpreter/eval_source.c",
            "src/interpreter/vm_control.c",
        },
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

    const vm_control_test = addCExecutable(
        b,
        "lainir-vm-control-test",
        "src/cli/vm_control_test.c",
        target,
        optimize,
    );
    vm_control_test.root_module.linkLibrary(core);
    vm_control_test.root_module.linkLibrary(text);
    vm_control_test.root_module.linkLibrary(interpreter);
    _ = installNamed(
        b,
        vm_control_test,
        "lainir-vm-control-test",
        "Check the opaque LainVM control plane",
    );
    // The two native host drivers are compiled outside Zig by the Python
    // scripts, which also generate the Lain C they link against:
    //   src/host/native_compiler.c -> scripts/run_lainir_self_host.py
    //   src/host/native_lainc.c    -> scripts/build_lainc_native.py
    //
    // The default build compiles the drivers themselves, which needs no
    // generated C, so a broken host still fails plain `zig build`.  The
    // executable steps further down also link the generated C, which only
    // exists once a script has produced it: they therefore stay off the
    // install step and require -Dgenerated-c=<path>.
    const native_compiler_host = addNativeHostLibrary(
        b,
        "lainir_native_compiler_host",
        "src/host/native_compiler.c",
        native_compiler_sources,
        native_c_flags,
        target,
        optimize,
    );
    const native_lainc_host = addNativeHostLibrary(
        b,
        "lainir_native_lainc_host",
        "src/host/native_lainc.c",
        native_lainc_sources,
        native_lainc_c_flags,
        target,
        optimize,
    );

    const native_hosts = b.step(
        "native-hosts",
        "Compile the native host drivers used by the Python build scripts",
    );
    native_hosts.dependOn(&native_compiler_host.step);
    native_hosts.dependOn(&native_lainc_host.step);
    b.getInstallStep().dependOn(native_hosts);

    const generated_c: ?std.Build.LazyPath = if (b.option(
        []const u8,
        "generated-c",
        "Script-generated Lain C to link into lainir-compiler / lainc-native",
    )) |path| generatedCSource(b, path) else null;

    addNativeHostExecutable(
        b,
        "lainir-compiler",
        "lainir-compiler",
        "src/host/native_compiler.c",
        native_compiler_sources,
        native_c_flags,
        generated_c,
        target,
        optimize,
    );
    addNativeHostExecutable(
        b,
        "lainc-native",
        "lainc",
        "src/host/native_lainc.c",
        native_lainc_sources,
        native_lainc_c_flags,
        generated_c,
        target,
        optimize,
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

/// Module for a native host driver: its link closure, the driver itself, and
/// optionally the C a Python script generated for it.
fn nativeHostModule(
    b: *std.Build,
    driver: []const u8,
    sources: []const []const u8,
    flags: []const []const u8,
    generated_c: ?std.Build.LazyPath,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) *std.Build.Module {
    const module = newCModule(b, target, optimize);
    module.addCSourceFiles(.{ .files = sources, .flags = flags });
    module.addCSourceFile(.{ .file = b.path(driver), .flags = flags });
    if (generated_c) |generated| {
        module.addCSourceFile(.{ .file = generated, .flags = flags });
    }
    return module;
}

/// Compile-only coverage for a native host driver, part of the default build.
fn addNativeHostLibrary(
    b: *std.Build,
    name: []const u8,
    driver: []const u8,
    sources: []const []const u8,
    flags: []const []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) *std.Build.Step.Compile {
    return b.addLibrary(.{
        .name = name,
        .root_module = nativeHostModule(
            b,
            driver,
            sources,
            flags,
            null,
            target,
            optimize,
        ),
        .linkage = .static,
    });
}

/// Standalone step producing a native host executable from script-generated C.
/// Deliberately not part of the install step: the generated C does not exist
/// until a Python driver produces it.
fn addNativeHostExecutable(
    b: *std.Build,
    step_name: []const u8,
    exe_name: []const u8,
    driver: []const u8,
    sources: []const []const u8,
    flags: []const []const u8,
    generated_c: ?std.Build.LazyPath,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) void {
    const step = b.step(step_name, b.fmt(
        "Build the native {s} executable from -Dgenerated-c",
        .{exe_name},
    ));
    if (generated_c) |generated| {
        const executable = b.addExecutable(.{
            .name = exe_name,
            .root_module = nativeHostModule(
                b,
                driver,
                sources,
                flags,
                generated,
                target,
                optimize,
            ),
        });
        step.dependOn(&b.addInstallArtifact(executable, .{}).step);
    } else {
        step.dependOn(&b.addFail(b.fmt(
            "zig build {s} needs the script-generated Lain C: pass -Dgenerated-c=<path>",
            .{step_name},
        )).step);
    }
}

/// `-Dgenerated-c` accepts a path relative to seed/ or an absolute one.
fn generatedCSource(b: *std.Build, path: []const u8) std.Build.LazyPath {
    return if (std.fs.path.isAbsolute(path))
        .{ .cwd_relative = path }
    else
        b.path(path);
}
