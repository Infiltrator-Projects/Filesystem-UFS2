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
