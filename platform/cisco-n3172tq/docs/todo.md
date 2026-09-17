# Cisco Nexus 3172TQ — what is left, in the order it has to happen

The board is `planned`: the discovery is done and none of it is proved. This is
the path from here to a switch that forwards, ordered so that each step's
failure is diagnosable with the step before it working.

## 0. Before leaving the switch powered on

**Find out what the fans do with nobody driving them.** ⚠ NOSaic declares no
platform HAL for this board, and declaring one is what starts the cooling loop
— so on a NOSaic boot the fans stay wherever the hardware leaves them at
power-on. On this chassis that is unknown. It may be full speed, which is loud
and safe; it may be a CCTRL default that assumes a supervisor is managing it,
which is neither.

Measurable from the vendor OS before anything else happens: `show environment
fan` reports the zone duty, and `show environment temperature` reports all four
sensors. Then read them again after a NOSaic boot, if a NOSaic boot happens.
The thresholds to judge against are in `board.yml` — the tightest is the Back
(D3) sensor's minor at **46 °C**.

**Note the second unit, or the lack of one.** There is one 3172TQ in this lab
and PSU 1 in it is dead (`NXA-PAC-500W`, serial `DCB2146L0KN`), so the chassis
is single-supplied. A power event during a session takes the box down with
nothing to hide it.

## 1. Which of the four PCH GbE ports is `mgmt0`

⚠ Cheap to answer, and expensive to discover on the hardware. Two of the four
are `8086:0438`, which mainline `igb` claims; two are `8086:0436`, which **no
Intel driver in 6.12 claims at all**. If `mgmt0` is one of the latter, a
NOSaic boot comes up with no management interface and nothing in `dmesg`
mentioning a network device — which reads as dead hardware.

From a root shell on the vendor OS, match `mgmt0`'s MAC (`b4:de:31:3f:a5:c0`)
against each function:

```sh
for d in /sys/bus/pci/devices/0000:01:00.[1-4]; do
    echo "$d $(cat $d/device) $(cat $d/net/*/address 2>/dev/null)"
done
```

If it is an `0x0436`, the fix is a one-line PCI ID addition to `igb` and it
belongs in `recipes/linux/patches/`. See
[hardware.md](hardware.md#the-management-port).

## 2. Generate the port map, before the first boot rather than after

`tools/mkportmap.sh` and `tools/mkpolarity.sh`, from one `config show` capture
off the vendor's SDK shell. The datapath will not start without the first, and
ports link and carry nothing without the second.

Take the capture **while the vendor OS is still installed**, because after an
install it is gone — and getting it back means putting NX-OS back on the box.
This is the one irreversible ordering constraint on the whole page.

Both generators are written against the vendor's output shape and neither has
ever been run. Check their port counts against the 54 they expect.

## 3. Boot our own kernel, from the EFI shell, without installing anything

The install is destructive and the boot path is unproven, so prove the boot
path first, off a USB stick, with the vendor OS still on the disk.

`Boot0003 "EFI USB Device"` is already an active boot entry and the EDK2 shell
is `Boot0002`, so a FAT stick with `\EFI\BOOT\BOOTX64.EFI`, `initrd.img` and
`startup.nsh` on it is bootable with no change to the switch at all. The three
files are exactly what `make image` puts on the ESP — copy them off the built
disk image with `mcopy`.

What this settles, in order of how much it would cost to learn later:

- [ ] Does the firmware launch our kernel at all, or does it repeat the loader's
      reset? The NBI attempt died between `CardIndex` and the kernel printing
      anything, and it is not established which side of that line the problem
      is on. **This is the single biggest open question about the board.**
- [ ] Does the EDK2 shell find `startup.nsh` by itself, or does it have to be
      run by hand? If it does not, step 5 below is not optional.
- [ ] Does the console come up at 9600 with `ignore_loglevel`?
- [ ] Does `usb-storage` see the internal flash, and does it come up as `sda`?
- [ ] Is `mgmt0` there, and which interface name does it get?

## 4. Install, and get back

- [ ] Save the NX-OS image off the box and verify its md5. There is exactly one
      copy on the chassis.
- [ ] Run the installer. Check the EFI system partition verification line.
- [ ] Boot NOSaic from the internal disk through the EFI shell.
- [ ] **Then immediately prove the way back** — netboot the NX-OS image with
      `loader> boot tftp://...` and `install all`. Recovery that has never been
      exercised is not recovery. The TFTP transport itself is proven on this
      hardware (5.3 MB at 3.7 MB/s across a subnet boundary); the reinstall is
      not.

## 5. A real firmware boot entry, instead of the shell

Today's arrangement is: the EDK2 shell is first in `BootOrder`, it auto-runs
`startup.nsh` from our EFI system partition, and the script launches the kernel
with a command line. It works because the shell passes arguments and a plain
`Boot####` entry does not.

The tidy ending is a `Boot####` entry whose **optional data carries the command
line as UCS-2**, so the firmware boots the kernel directly. `CONFIG_EFIVAR_FS`
is already built in for it. Two routes:

- `efibootmgr` from the running switch, which is a package NOSaic does not
  currently build. It is small and it is the right answer.
- The shell's `bcfg boot -opt <index> <file>`, with a file containing the
  command line already encoded UCS-2. Works from a console session and has to
  be redone after a firmware reset.

Until one of those is done, **do not** replace the shell entry with a direct
`bcfg boot add` — a kernel launched that way gets no `console=` and no
`initrd=`, and the symptom is a completely silent boot.

## 6. Bring up the datapath

Everything above is about getting a kernel and a userspace onto the box.
`nosd-td2` is where the board becomes a switch.

- [ ] `nosaic show caps`. Compare against the 7050TX-64's `contract 1, ports 8
      max` — this board should be no worse, and if it is, the difference is
      board data rather than driver.
- [ ] The 48 copper ports, which is where `datapath/td2/phy.c` gets its second
      test. The BCM84848 PHYs need firmware downloaded over MDIO at init, the
      MDIO addresses are **swapped in pairs**, and physical lane numbering
      **skips 17–20**. Each of those fails silently and differently.
- [ ] The six 40G cages, which are **retimed through BCM84328s** rather than
      direct-attach. That is new here: the Arista's cages are direct SerDes.
      Whether the SDK's `phy_84328_<n>` handling wants anything from us beyond
      the generated port map is unknown.
- [ ] `config/portmode.conf` — prove one cage broken out into 4 × 10G.
- [ ] ACL. `datapath/common/acl.c` exists, `datapath/td2` has no field-group
      support wired to it, and this chip's ingress FP is **4096 entries** —
      twice Trident+'s. The prediction on record is ~2560 IPv4 + ~1536 IPv6, or
      ~3500 v4-only, and it is expected to come out lower because the vendor
      reserves ingress slices for its own features. Falsifiable the moment the
      capability exists; record whichever way it falls.

## 7. A platform HAL

The largest remaining piece, and the board is useful without it.

- [ ] **Map the mux `0x70` channels to devices.** Watch `dmesg` on the vendor
      OS while touching each subsystem — successful selections are not logged,
      only failures, so this is done by elimination. Without the channel map an
      i2c map in `board.yml` would be a guess that reads a device that is not
      there.
- [ ] **Decode the SPROM** for the per-board thermal thresholds, the MAC base
      and the serial. Dumps are in the RE repository's `analysis/hw/`. This is
      what would let the switch state its own identity from hardware rather
      than from configuration.
- [ ] **Sensors**, four of them, enumerated by CCTRL rather than probed.
- [ ] **Fans**, in zones, with a PWM floor — and worth copying the vendor's
      **tachometer feedback**, which NOSaic does not do on any board. Commanding
      a duty and checking the RPM agrees is the difference between noticing a
      seized fan and not.
- [ ] **PSU presence and health.** The vendor models presence, health,
      redundancy mode and the PSU's own fan separately; ours is a GPIO word
      giving presence. The dead supply in this unit makes it testable.
- [ ] **QSFP EEPROM**, which on this board is reached through the **ASIC's CMIC
      I²C**, not through the board controller. ⚠ This is the inverse of the
      Arista arrangement and it is the easiest thing here to implement
      backwards.
- [ ] **Chassis LEDs.** 32-bit MMIO over the PLX PCI9030's local-bus windows.
      Which offsets are the LEDs is not known and needs either a live read of
      BAR2–5 or the register map out of `libcrdcfgdatan3k_qz2.so`. Front-panel
      *port* LEDs are the ASIC's and the SDK already drives them.

## Not ours to finish, but worth recording

**Why the NBI route resets after `CardIndex`.** NOSaic does not need it — the
firmware boots our kernel directly — but the answer is in the RE repository's
open threads, and if step 3 shows the firmware path failing the same way, the
two questions turn out to be one.

Candidates on record: the header's exec address (`0x92800`) points at mknbi's
`first32pm` stub, which neither our image nor Cisco's contains, so their loader
must take a different path for an image it recognises; or `Image valid` implies
a validation step we do not satisfy whose failure path resets rather than
printing.
