# sl-disk: a ready Mac OS X 10.6.8 disk from your own install DVD

`make-sl-disk.sh` turns your own Mac OS X 10.6 install DVD image and Apple's 10.6.8 combo update
into a never-booted 10.6.8 (10K549) system disk for QEMU. Its first boot shows the Welcome screen
and the Setup Assistant, as on a new Mac. Nothing is typed or clicked: the install runs headless.

```
make-sl-disk.sh --dvd "Mac OS X Install DVD.dmg" --combo MacOSXUpdCombo10.6.8.dmg \
                --opencore moose-ripple-VERSION.qcow2 --osk-file smc-key.txt osx-10.6.8.qcow2
```

- `--dvd`: a Mac OS X 10.6 install DVD (10.6.0 to 10.6.3) as `.dmg`, or a raw image of the disc.
- `--combo`: `MacOSXUpdCombo10.6.8.dmg`.
- `--opencore`: moose-ripple's release disk; it boots the installer and is only read.
- `--osk-file`: a file holding the SMC key of a real Mac on one line.
- `--work DIR`: scratch folder, about 25 GB (default: `sl-disk-work` next to the output). Deleted
  at the end unless `KEEP_WORK=1`; it also holds a screenshot every 2 minutes, to see a stall.
- `--ripple`: also install moose-ripple onto the new disk (`install.sh --target`, from the package
  on the release disk's RIPPLE volume), so its first boot, the Welcome animation included, has it.
  `--ripple-options "--sound --hwcursor"` passes `install.sh`'s options (`--sound`, `--hwcursor`,
  `--no-tablet`, `--logs`). Needs a release disk newer than 0.1.0 (its `install.sh` has `--target`).

Needs a Linux host (or WSL2) with KVM, `qemu-system-x86_64`, `qemu-img`, OVMF, 7-Zip (`7z`) and
perl. No root. Tested on Debian 13 (QEMU 10.0) with the 10.6.3 retail DVD: about 10 minutes,
and the disk boots to the Welcome screen. With `--ripple`, the Welcome movie played smoothly at
its first start in UTM (Apple's software renderer manages a few frames a second there).

## How it works

1. The DVD's own startup script, `/etc/rc.install`, has an unattended mode, used by Apple's
   NetInstall. A copy of the DVD image gets `rc.install` from this folder in its place,
   written over the original's bytes at the same length, so no HFS+ driver and no root are
   needed.
2. Pass 1: the VM boots the DVD through OpenCore. Our `rc.install` partitions the empty disk
   (GUID, Mac OS Extended Journaled), runs Apple's installer with an automation file, and the
   installer restarts the VM at the end, which ends QEMU.
3. Pass 2: the DVD boots again (fresh firmware settings, so OpenCore picks the DVD and never the
   new system). Our `rc.install` sees the installed system and applies the combo update with
   that system's own `installer`.
   With `--ripple`, it then runs the release disk's `install.sh --target` on the new system.
4. It writes `SLREADY OK 10K549` and the installer's log to a small result disk and powers off;
   the script checks that line and compresses the disk.

The VM runs with one CPU: with two, 10.6.3's installer kernel sometimes panicked at boot with
`pmap_flush_tlbs() timeout`.
