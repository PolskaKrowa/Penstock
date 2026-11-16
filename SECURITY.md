# Security Policy

## Scope

This policy applies to the Penstock repository and the software it contains, including:

- the Penstock compiler and the Freshet source-language front end (`.fsht` files);
- preprocessing, `#include`, module imports and project configuration;
- LLVM IR generation, optimisation passes, the optional LLVM `opt` plugin, and the Clang/LLVM backend integration;
- the embedded Z3 and Alive2 superoptimisation and verification components;
- the Freshet runtime, public `libfsh` C API, and parallel execution facilities;
- the `freshet-language-server`, editor integrations, build scripts and CI configuration.

Security reports concerning vulnerabilities in bundled or fetched third-party dependencies are also in scope
when they affect Penstock's build, distribution or operation. Where a report is solely about an upstream dependency,
please identify the affected dependency and version or revision so that it can be routed appropriately.

## Supported versions

Penstock is currently identified by the project as `0.1.0-INDEV`. The repository does not currently publish a
formal release-support or backport schedule.

Please report issues against the latest commit on the [`master` branch](https://github.com/PolskaKrowa/Penstock/tree/master), including the exact commit SHA where possible.
Fixes will be prioritised for the current development line. Whether a fix is backported to an older release
or revision will be considered on a case-by-case basis.

## Reporting a vulnerability

**Please do not report suspected vulnerabilities in a public GitHub issue, discussion, pull request or commit.**
Public reports can expose users to an exploitable issue before a fix is available.

1. First, use GitHub's private vulnerability reporting facility from the repository's [Security tab](https://github.com/PolskaKrowa/Penstock/security) by selecting **Report a vulnerability**, if that option is available.
3. If private vulnerability reporting is unavailable, contact the repository owner privately through the [maintainer's GitHub profile](https://github.com/PolskaKrowa) and ask for a private channel for the report. Do not include exploit details in a public message.

A dedicated security email address is not documented in the repository at present. Please use one of the
private routes above rather than guessing an address or posting technical details publicly.

### What to include

Please provide as much of the following as you can safely share:

- a concise description of the suspected vulnerability and its potential impact;
- the affected Penstock version, commit SHA, dependency, and build configuration;
- the operating system, architecture, compiler/backend version and relevant environment settings;
- precise steps to reproduce the issue, including the command line and a minimal `.fsht` input or project where practical;
- the observed behaviour and the behaviour you expected;
- relevant diagnostics, logs, crash traces or sanitiser output;
- any known workaround or mitigation.

Keep proof-of-concept material as small as possible. Do not include credentials, private source code, personal
information or other sensitive data unless it is essential and you have agreed a secure transfer method with the maintainer.

## Examples of security issues in scope

Examples include, but are not limited to:

- memory corruption, out-of-bounds access, use-after-free, integer overflow or other unsafe behaviour in the compiler, preprocessor, parser, optimiser, runtime or `libfsh` when processing crafted input;
- a crafted source file, project or workspace causing unintended file access, path traversal, disclosure of files outside the intended workspace, or unsafe writes;
- command or argument injection, or unintended execution, through compiler options, project files, preprocessor directives, linker arguments or backend invocation;
- vulnerabilities in the LSP server that allow a crafted document or protocol message to read or modify unintended resources, crash the server, or affect other workspaces;
- a compiler defect that causes generated code to violate memory-safety assumptions in a way that can be triggered by attacker-controlled source;
- denial of service through pathological source files, imports, macro expansion, optimiser input, resource exhaustion or unbounded processing;
- unsafe handling of temporary files, caches, profiling output, build artefacts or dependency downloads;
- a compromised or vulnerable dependency, submodule, build tool or downloaded toolchain that affects Penstock users.

A normal compiler diagnostic, performance issue, unsupported language feature or ordinary crash without a plausible
security impact is not necessarily a security vulnerability. Such issues can be reported through the regular [issue tracker](https://github.com/PolskaKrowa/Penstock/issues).

## Important security and usage notes

**Penstock is a compiler and toolchain, not a security boundary or sandbox.** In particular:

- Freshet programs can call native functions through FFI and link native libraries using `#link` declarations. Those libraries execute with the privileges of the process that runs the resulting program.
- The `penstock --run` workflow executes the compiled program on the host. Treat untrusted source code as untrusted executable code; do not use `--run` on source you do not trust.
- Source preprocessing and module loading read files from the input's directory, configured include directories and the built-in module directory. Do not assume that compiling an untrusted project confines file access to that project's directory.
- Penstock invokes external backend tools, including Clang and platform link/archive tools. The compiler does not make those tools, the host operating system or third-party libraries trustworthy.
- The default CMake configuration can download or build LLVM, Z3, Alive2 and other dependencies. Use trusted toolchains and dependency sources, review submodule revisions, and build in a controlled environment.

When analysing untrusted source or building untrusted projects, use an appropriately isolated virtual machine or container,
an unprivileged account, restricted filesystem mounts, no access to secrets, and suitable CPU, memory and time limits. Disable
network access unless it is required. Keep the compiler, backend and operating system patched. Do not rely on Penstock's compile-time
checks or optimisation verification as a substitute for process isolation.

## Triage and disclosure

The maintainer will review private reports, assess severity and affected versions, and coordinate a fix or mitigation with
the reporter where appropriate. Please allow reasonable time for investigation and remediation before public disclosure.

There is no published response-time or remediation-time guarantee. The maintainer will aim to acknowledge reports and provide
updates as practicable. Once a fix or mitigation is available, the maintainer and reporter can agree an appropriate disclosure
date and the information to publish. Reporter credit can be included by agreement.

## Existing security-related checks

The repository currently contains GitHub Actions workflow definitions for CodeQL analysis (including C/C++, Python and GitHub Actions)
and Codacy scanning, alongside build and test workflows. The CMake bootstrap also verifies the SHA-256 digest of the downloaded LLVM
release archive against the digest published in GitHub release metadata.

These checks reduce some risks but are not a guarantee that the code or its dependencies are free from vulnerabilities. Please continue
to report suspected security issues privately, including issues that appear to bypass or evade these checks. Any vulnerabilities in the
Penstock source code which bypassed the checks provided by Github and Codacy will also be forwarded to the appropriate developers of the
checks after the appropriate vulnerability has been fixed in Penstock.
