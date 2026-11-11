# UFS migration status
The Defragmenter UFS native reader, analyser and relocation worker have
been brought into the independent UFS.so source tree. Its format-specific
algorithms run behind a typed entry point, and the generic device, journal,
Stop and exact-I/O host services remain owned by Defragmenter/Common.
The UFS module's standalone build and ABI test are required by CI.
**Not yet an end-to-end UFS Defragmenter migration** until the application
loader is switched and full disposable-image Defrag/Growth/Recover verification
has passed. UFS test-media formatting still needs relocation to its driver.

The first-party UFS2 Test Media format/populate engine was moved verbatim
from Defragmenter into fixtures/ufs_mkfs.c and is now compiled into UFS.so.
The module exports an independent format_test_media entry. Defragmenter
will retain only the generic module-host launcher for this operation.

## UFS1 / UFS2 independent SO split (2026-10-31)

The original repository was renamed to Filesystem-UFS2, preserving history.
The former shared UFS.so becomes UFS2.so, with an explicit UFS2-only
on-disk identity guard and format_id=ufs2. UFS1 is published independently
in Filesystem-UFS1/UFS1.so. Existing UFS2-only Test Media format and raw
payload interfaces remain solely in this repository. Full qualification and
consumer coordination are mandatory before replacing deployed UFS.so.

## Common V2 read-only allocation verification (1.0.15)
The hosted CORE mount and VOLUME query now verify cylinder-group bitmap
free totals through Common V2 read-only I/O. Damaged group metadata,
inconsistent allocation counts and misaligned 4Kn requests fail closed.
The native UFS2 parser and verifier remain in this independent source repo.
These checks are not full filesystem repair, file traversal, or qualified
V2 Defrag map/plan/execute/recover. Those capabilities remain unadvertised.
