# UFS2 Common V2 migration status

Hosted UFS2.so implements real native UFS2 LE/BE geometry, cylinder-group
bitmap validation, read-only inode and directory inventory, V2 STAT, LIST,
READ and actual byte-range file content through the partition-relative host,
plus its original real DEFRAG.MAP functionality. Disposable native UFS2
fixtures exercise these functions, invalid paths and media failures.

The installed production .so links only the native engine and Common V2
adapter. The old Linux VFS adapter, worker, formatter and legacy V1 code remain
as source, but are not linked to production. The two-file Debian and source
RUN qualification workflows do not publish Linux .ko substitutes.

The current module is **NOT a BOOT-qualified release**. A freestanding ELF
using the shared engine, unmodified production BOOT validator acceptance,
and OVMF kernel/initramfs reads with corrupted media and nonmutation checks
remain necessary. The hosted reader's single mounted-volume session is
also a known limitation requiring follow-up independent-session support.
