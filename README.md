# Filesystem-UFS2 — UFS2.so

Independent UFS2 on-disk filesystem driver for InfiltratorOS, descended from
the original Filesystem-UFS repository. Only UFS2 little- and big-endian
volumes are accepted; UFS1 belongs to [Filesystem-UFS1](https://github.com/Infiltrator-Projects/Filesystem-UFS1).

`UFS2.so` owns the UFS2 parser, analyser, staged Defrag/Growth Defrag,
Recover, first-party UFS2 Test Media formatter, validation and raw payload
verification. It installs to `/System/Filesystems/UFS2.so`.

`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build --output-on-failure`

The V1 symbol names describe compatibility of the *software API*, not the
on-disk UFS generation. UFS1 and UFS2 may share a historical C source
lineage, but are now separately identified, built and installed.

**Qualification:** acceptance/rejection tests exercise on-disk magic and the
existing UFS2 Test Media suite. Independent real-media and recovery
qualification must pass before calling either module fully complete.

## Installer files

- `/System/Filesystems/UFS2.so`
- `/System/Resources/Documentation/Filesystem-UFS2.pdf`
- `/System/Kernel/Filesystems/ufs2.ko`

The published Debian package installs these three regular files. A manual native RUN compiles the shared object and Linux adapter locally, generates the PDF, and replaces all three files. It uses the running kernel headers by default. Use `--kernel-build DIRECTORY` for a prepared kernel build with `Module.symvers`. The kernel module is stored for manual use.

InfiltratorOS calls `--system-package-mode` and decides which outputs need updating. Its worker builds a missing or older SO, repairs the PDF independently, and stores the matching released kernel module when available. Use `--build-only --output DIRECTORY` to build a Debian package.
