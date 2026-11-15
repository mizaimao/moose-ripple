"""Write SSDT-HPET.aml: the HPET's resources with the legacy interrupts 0 and 8, as on a Mac.

    DefinitionBlock ("", "SSDT", 2, "RIPPLE", "HPETIRQ", 1)
    {
        Scope (\\_SB.HPET)
        {
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadOnly, 0xFED00000, 0x00000400)
                IRQNoFlags () {0}
                IRQNoFlags () {8}
            })
        }
    }

QEMU's HPET lists only its registers. Mac OS X 10.6 then gives its timers 1 and 2 the I/O APIC
inputs 16 and 17 (edge-triggered), which are the PCI interrupt lines A and B of QEMU's Q35
machine; PCI devices on those lines fall back to the mirrored PC interrupt 10 (see README, PCI
interrupts). config.plist renames the DSDT's HPET _CRS to XCRS, so this one takes its place, and
takes IRQ 8 out of the RTC's resources, as on a Mac.

Assembled here by hand (iasl isn't needed), byte for byte the ASL above. Run on dev.lan in the
repo's .venv:
    .venv/bin/python -I boot/opencore/acpi/ssdt-hpet.py boot/opencore/acpi/SSDT-HPET.aml
"""
import struct
import sys

HPET_BASE, HPET_LEN = 0xFED00000, 0x400

res = (bytes([0x86, 0x09, 0x00, 0x00]) + struct.pack("<II", HPET_BASE, HPET_LEN)  # Memory32Fixed RO
       + bytes([0x22, 0x01, 0x00])        # IRQNoFlags {0}
       + bytes([0x22, 0x00, 0x01])        # IRQNoFlags {8}
       + bytes([0x79, 0x00]))             # EndTag
buf = bytes([0x0A, len(res)]) + res      # BufferSize as ByteConst, then the bytes
buf = bytes([0x11, 1 + len(buf)]) + buf  # BufferOp, PkgLength (one byte: under 64)
body = b"\\\x2e_SB_HPET" + b"\x08_CRS" + buf  # \_SB.HPET (DualNamePrefix), NameOp _CRS
assert 1 + len(body) < 64
aml = bytes([0x10, 1 + len(body)]) + body   # ScopeOp, PkgLength

table = bytearray(b"SSDT" + struct.pack("<I", 36 + len(aml)) + bytes([2, 0]) + b"RIPPLE"
                  + b"HPETIRQ " + struct.pack("<I", 1) + b"RPL " + struct.pack("<I", 1) + aml)
table[9] = -sum(table) & 0xFF            # checksum: all bytes add up to 0
assert sum(table) & 0xFF == 0

with open(sys.argv[1], "wb") as f:
    f.write(table)
print(f"{sys.argv[1]}: {len(table)} bytes")
