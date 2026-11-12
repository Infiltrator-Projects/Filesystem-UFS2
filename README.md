# Filesystem-UFS2 — UFS2.so

Independent UFS2 on-disk filesystem driver for InfiltratorOS, descended from
the original Filesystem-UFS repository. The native decoder recognises UFS2
little- and big-endian volumes. Common V2 discovery covers both
variants using the format-owned read-only decoder. UFS1 belongs to [Filesystem-UFS1](https://github.com/Infiltrator-Projects/Filesystem-UFS1).

`UFS2.so` owns the UFS2 parser, analyser, staged Defrag/Growth Defrag,
Recover, first-party UFS2 Test Media formatter, validation and raw payload
verification. It installs to `/System/Filesystems/UFS2.so`.

`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build --output-on-failure`

UFS1 and UFS2 are separately identified, built and installed. The
package's required filesystem entry point is the Common V2 API.
Older format-specific exports are not part of the target architecture and
must be retired after their caller functionality has a qualified native V2
implementation, not silently retained as the long-term interface.

**Qualification:** acceptance/rejection tests exercise on-disk magic and the
existing UFS2 Test Media suite. Independent real-media and recovery
qualification must pass before calling either module fully complete.

## Installer files

- `/System/Filesystems/UFS2.so`
- `/System/Resources/Documentation/Filesystem-UFS2.pdf`
- `/System/Kernel/Filesystems/ufs2.ko`

The published Debian package installs these three regular files. A manual native RUN compiles the shared object and Linux adapter locally, generates the PDF, and replaces all three files. It uses the running kernel headers by default. Use `--kernel-build DIRECTORY` for a prepared kernel build with `Module.symvers`. The kernel module is stored for manual use.

InfiltratorOS calls `--system-package-mode` and decides which outputs need updating. Its worker builds a missing or older SO, repairs the PDF independently, and stores the matching released kernel module when available. Use `--build-only --output DIRECTORY` to build a Debian package.

## Common V2 integration (read-only phase)

`UFS2.so` exports the native `infiltrator_filesystem_api_v2` descriptor
with `base_v1 == NULL` and a real CORE probe over partition-relative Common
host I/O. It does not open devices or execute any mutation while probing.
Both UFS2 endianness variants are recognised in this Common V2 phase.
Mount, file operations, formatting, repair, defragmentation, and kernel or EFI
execution are **not** yet exported through the Common V2 groups. The
format-specific legacy interfaces remain for existing qualified consumers.

## Native build and release

UFS2 uses **C, C++17 and native shell** for independent package building,
the PDF reference, two-pass PGO, kernel adapter compilation and release
qualification. There are no Python scripts in the standalone toolchain.

`bash standalone/release.sh release` creates exactly one portable
Debian package and one source-embedded native installer RUN. The RUN
verifies its payload hash before extracting and compiling locally.
`bash standalone/release.sh install --profile aggressive --build-only --output DIRECTORY --system-package-mode`
qualifies a hardware-specific package without modifying installed
system files. `--kernel-build DIRECTORY` requires an externally
prepared kernel build with `Module.symvers`.

The installed module remains Common ABI V2. The older per-format
Defragmenter entry points are not a V2 operation group and are still
tracked for replacement rather than misrepresented as native V2.
