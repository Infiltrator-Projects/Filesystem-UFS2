# UFS migration status
The Defragmenter UFS native reader, analyser and relocation worker have
been brought into the independent UFS.so source tree. Its format-specific
algorithms run behind a typed entry point, and the generic device, journal,
Stop and exact-I/O host services remain owned by Defragmenter/Common.
The UFS module's standalone build and ABI test are required by CI.
**Not yet an end-to-end UFS Defragmenter migration** until the application
loader is switched and full disposable-image Defrag/Growth/Recover verification
has passed. UFS test-media formatting still needs relocation to its driver.
