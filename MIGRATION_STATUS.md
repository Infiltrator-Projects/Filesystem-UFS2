# UFS2 Common V2 migration and BOOT qualification

The canonical installed UFS2.so uses the same native source-owned UFS decoder
and Common V2 callbacks for its hosted and freestanding profiles. The complete
no-import freestanding ELF is embedded in the hosted shared object's
.infiltrator_boot section. The module exposes only the Common V2 ABI.

The native reader performs real partition-relative PROBE/MOUNT/UNMOUNT,
STAT/LIST/READ and DEFRAG.MAP through bounded, read-only Common V2 hosts.
Multiple mounted volumes use independent session tokens and inode inventories.
Historical analyser and writer sources remain available without supplying a
runtime V1, kernel .ko, FUSE or Linux VFS fallback.

The unchanged BOOT 0.1.77 production validator accepts the standalone ELF
and installed shared object. Disposable OVMF tests read a real EFI-stub Linux
kernel and initramfs through UFS2.so, verify the initramfs marker, refuse
a corrupted superblock and check source-media nonmutation byte-for-byte.
The BOOT staging directory is EFI/BOOT/drivers_x64.

A new release requires the portable and CPU-native source RUN shared objects
to pass the same validator and all hosted and OVMF jobs to pass for the
identical main commit. Physical Secure Boot/MOK policy remains an independent
hardware/environment gate; no machine configuration is changed by these tests.
