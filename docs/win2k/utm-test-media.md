# Change test media in UTM for Windows XP and 2000

Use this procedure when the guest keeps seeing the old CD or a live QEMU
media change reports `Operation not permitted`. In the XP test, the old
CD was a fixed image inside the VM bundle. With the VM stopped, UTM
attached the existing test image to a new removable drive. XP then read
a file unique to the new disc from `D:\`.

## Replace a fixed CD

1. Shut Windows down and confirm UTM shows **Stopped**. Identify the
   Windows boot disk and the old CD in **Edit → Drives**. Keep the source
   of the old CD. UTM can delete its bundled copy when a fixed drive is
   removed. If that copy matters, compare it with the retained source
   using `cmp` before removing the drive. [UTM drive settings](https://docs.getutm.app/settings-qemu/drive/drive/)
2. Add an **IDE** drive with **Removable** selected and choose the new
   ISO through UTM. Remove the old CD entry once the replacement is
   attached. Keep the Windows disk first and the new CD second. On the
   tested i440fx configuration, starting with a third IDE entry failed
   with `Bus 'ide.2' not found`.
3. Boot Windows. Read a file unique to the new build. For example, with a
   serial log attached, `cmd /c type D:\C16QA.CMD >COM1` returned the
   16-bit test script from the replacement CD. Use the guest's actual
   drive letter. A drive listed in UTM alone does not verify the read.

### From a shell on the UTM host

UTM exposes the same operation through AppleScript. The example takes the
VM name, the expected boot disk ID, the expected old CD ID and the absolute
path to the existing ISO. It checks the two-drive layout and uses UTM to
save both configuration changes while the VM is stopped. The drive IDs are
the `Identifier` fields in the VM's `config.plist` under `Drive`.
[UTM scripting examples](https://docs.getutm.app/scripting/cheat-sheet/#configuration)

```sh
osascript - "$vm_name" "$boot_id" "$old_cd_id" "$iso_path" <<'APPLESCRIPT'
on run argv
    set {vmName, bootID, oldCDID, imagePath} to argv
    set testDisc to POSIX file imagePath
    tell application id "com.utmapp.UTM"
        set vm to virtual machine named vmName
        if status of vm is not stopped then error "Shut down the VM first"
        set cfg to configuration of vm
        set ds to drives of cfg
        if (count ds) is not 2 then error "Expected one boot disk and one CD"
        set bootDisk to item 1 of ds
        set oldCD to item 2 of ds
        if id of bootDisk is not bootID or id of oldCD is not oldCDID then error "Drive IDs changed"
        update configuration of vm with {drives:{{id:bootID}, {id:oldCDID}, {removable:true, interface:IDE, source:testDisc}}}
        set cfg to configuration of vm
        set ds to drives of cfg
        if (count ds) is not 3 then error "Replacement drive was not added"
        set newCD to item 3 of ds
        if removable of newCD is not true then error "Expected a removable drive"
        update configuration of vm with {drives:{{id:bootID}, {id:(id of newCD)}}}
    end tell
end run
APPLESCRIPT
```

If UTM itself refuses the file, stop and have the owner select it through
UTM's **Browse** picker. Keep the original image and macOS permissions
intact.

## Test a folder of loose driver files

XP also read a small FAT16 data disk in the second IDE slot. Use this
when the current build is a folder. On macOS, the following commands
create a 32 MiB disk with an MBR partition table. Set `build_dir` to the
driver folder and `scratch` to an empty test directory. The final copy
takes just the sectors of the disk from the uncompressed Mac image.

```sh
hdiutil create -size 32m -layout MBRSPUD -fs 'MS-DOS FAT16' \
  -volname XPDRIVER -nospotlight -format UDRW -srcfolder "$build_dir" \
  -o "$scratch/driver.dmg"
dd if="$scratch/driver.dmg" of="$scratch/driver.img" bs=1m count=32
```

Shut down the VM, detach the removable CD and import `driver.img` as an
IDE **Disk Image** after the boot disk. For a disposable driver test, start
the VM with `utmctl start "$vm_name" --disposable`. An in-guest restart
lets Windows load the driver. Shutting down ends the disposable session.

The Windows 2000 SP4 test guest read the FAT disk on IDE. Its default
installation did not mount the same disk on the emulated LSI SCSI
controller. To keep a fixed CD with the VM, temporarily move that CD to
SCSI and put the test disk on IDE. Windows 2000 then read the test files
from `D:\`. After shutdown, return the original CD to IDE and remove the
temporary disk.

Inside Windows, run `NT5INST.EXE` from the test drive and restart. Compare
the two installed files against the same drive before running any graphics
checks. Substitute its drive letter in these commands.

```bat
fc /b D:\rb2kmp.sys %SystemRoot%\system32\drivers\rb2kmp.sys
fc /b D:\rb2kdisp.dll %SystemRoot%\system32\rb2kdisp.dll
```
