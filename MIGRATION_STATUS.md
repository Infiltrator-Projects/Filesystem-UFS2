# UFS migration — 2026-10-30

Native UFS analyser/worker source copied without modification from Infiltrator-Projects/Defragmenter commit 30af892d8897d3aeaeefeac1bfcdb9620a0a5c72. This is a **source snapshot**, not a separately functional shared object, kernel module or EFI driver. The worker still depends on Defragmenter's host interfaces; its existing implementation remains active there until a separately tested Common-ABI filesystem module is integrated.

UFS1 and UFS2 remain variants of this one UFS family repository. Test their separate on-disk rules and qualified write limitations before exposing standalone operations.
