# Filesystem-UFS2

## 1. Purpose and format scope

Unix File System UFS2, distinct from UFS1. This repository owns the format-specific implementation of `UFS2.so` for InfiltratorOS.

## 2. On-disk architecture and invariants

UFS2 superblock, cylinder-group geometry, wider inode block references and allocation maps are decoded in the appropriate native byte order. The inventory must distinguish actual host I/O errors from invalid structures and must not cache partial results.

## 3. Common V2 implementation and capabilities

Current Common V2 source declares probe, mount, stat, read and format; older probe-only prose is historical. Historical analyser, staged relocation, recovery and test-media internals are separate from the public V2 operation masks. The canonical entry point is `infiltrator_filesystem_api_v2`. Consumers must negotiate the actual profile and operation masks and use bounded, partition-relative host I/O. A source-level callback does not by itself establish that every format variant or host profile is qualified.

## 4. Integrity, mutation, allocation and recovery

Both endian variants require independent media tests. Fault injection must precede lazy inventory caching. Do not equate source-retained defragmentation with a qualified V2 relocation interface. For destructive operations, require the applicable exclusive writable host, durability contract and format-specific validation. Never silently substitute a kernel or FUSE driver or treat a rejected or unavailable operation as success. Preserve original disk-format semantics; relocation and recovery are separate from ordinary file I/O.

## 5. Build, installation and release

The target installation is `/System/Filesystems/UFS2.so` and `/System/Resources/Documentation/Filesystem-UFS2.pdf`. The project release policy is a portable amd64 DEB and a CPU-native source-first RUN, with GitHub's automatic source archive; do not treat this policy statement as verification of the current installer payload. The release workflow is the authoritative executable build and package specification. Historical adapters and test artifacts are not additional production filesystem authorities.

## 6. Qualification and limitations

[Recorded qualification workflow](https://github.com/Infiltrator-Projects/Filesystem-UFS2/actions/runs/45526891311) · [Current workflows](https://github.com/Infiltrator-Projects/Filesystem-UFS2/actions?query=branch%3Amain). A successful workflow establishes the tested scope only, not complete original-format parity, physical-media crash safety, or production BOOT/kernel integration. Use disposable images for destructive qualification.

## 7. Technical references and provenance

- [Provider implementation](module/ufs_module.c)
- [Native design or source provenance](native/filesystems/ufs/LINUX-SOURCE-PROVENANCE.md)
- [Migration and source history](MIGRATION_STATUS.md)
- [Original-format reference source](docs/UFS2.pdfstreams) and [PDF](docs/UFS2.pdf)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

The original-format reference and historical migration notes describe different evidence from the current production provider. When descriptions disagree, the current exported descriptor, implementation, executable tests and exact-revision release evidence take precedence.

The [engineering reference](docs/UFS2-TECHNICAL-REFERENCE.md) adds field-level original-format guidance. The PDF and editable reference contain one integrated manual.
