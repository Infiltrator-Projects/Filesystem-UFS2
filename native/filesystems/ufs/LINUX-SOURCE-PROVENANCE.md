# Linux UFS driver source provenance

This repository embeds the Linux UFS VFS adapter originally imported and retained in `Infiltrator-Projects/Filesystem-UFS1`, including its original file-level notices and copyright. The 15 source files under `native/filesystems/ufs/linux/` are a complete source-owned Linux UFS module tree. The Linux UFS VFS driver recognises both UFS1 and UFS2 disk structures; each independent userspace `UFS1.so` or `UFS2.so` remains owned, built and tested by its own format repository.

This source copy replaces an older dependency on downloading `Infiltrator-Projects/Filesystems` at `167bd9a1dae6d8f5842b7208703fb302e1f5b765` during the native installation. Copying it is not a claim of different on-disk algorithms or proof of independent runtime, remount or recovery qualification. The source files' original licence statements and attribution govern the imported Linux driver.
