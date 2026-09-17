# Cisco Nexus 3172TQ — what is left, in the order it has to happen

The board is `planned`: the discovery is done and none of it is proved. This is
the path from here to a switch that forwards, ordered so that each step's
failure is diagnosable with the step before it working.

## 0. Before leaving the switch powered on

**Find out what the fans do with nobody driving them.** ⚠ NOSaic declares no
platform HAL for this board, and declaring one is what starts the cooling loop
— so on a NOSaic boot the fans stay wherever the hardware leaves them at
power-on. That has never been observed.

The vendor's idle baseline *is* now measured, on the lab chassis:

| | |
|---|---|
| Fan zone 1 duty | `0x28` |
| ASIC die | 56 °C (minor 100, major 110) |
| Front-Left (D1) | 38 °C (minor 60, major 70) |
| Front-Right (D2) | 37 °C (minor 56, major 70) |
| Back (D3) | 31 °C (minor 46, major 70) |

So the vendor does **not** run these fans flat out, and the idle margin is
large — the tightest sensor, Back (D3), is 15 °C below its minor threshold.
That is reassuring rather than sufficient: the die was at 56 °C with no
datapath running, and `nosd` will make it hotter.

What is still unknown is the *unmanaged* duty, and there are only two ways to
learn it. Reading the fan controller from the loader's `smb` command needs the
controller's SMBus address, which is behind the unmapped mux. The other way is
item 1 below, which is why it comes first.

**Note the second unit, or the lack of one.** There is one 3172TQ in this lab
and PSU 1 in it is dead (`NXA-PAC-500W`, serial `DCB2146L0KN`), so the chassis
is single-supplied. A power event during a session takes the box down with
nothing to hide it — and it has already happened once: the box power-cycled on
2026-09-17 for "possible power loss", which is also how it lost
`feature bash-shell`.

## 1. Get the loader's handoff to work — the one thing between here and a boot

⚠ **This is the whole remaining blocker, and it is now narrow.**

Netbooting through the **loader's own TFTP** works as far as it possibly can
without executing: the NBI container is accepted, the kernel loads, the
initramfs loads, our command line reaches the kernel parameters, and the loader
calls `big_linux_boot`. Then the board resets with no kernel output, not even
`earlyprintk`.

```
Loading kernel length 14246400
Kernel loaded successfully
Loading intird 67579836
big_linux_boot
(c) Copyright 2018, Cisco Systems.     <- reset
```

Three causes are eliminated — the exec address (the vendor's image has the same
one), `setup_sects` (the vendor's kernel has the same situation), and KASLR
(`nokaslr` fails identically). See
[hardware.md](hardware.md#three-hypotheses-tested-and-eliminated).

- [x] ~~Build a kernel with `CONFIG_RELOCATABLE=n` and
      `CONFIG_PHYSICAL_START=0x100000`~~ — **ruled out without building it.**
      The vendor kernel's own config, extracted from its `IKCFG_ST` blob, is
      `CONFIG_RELOCATABLE=y`, `CONFIG_PHYSICAL_START=0x1000000`,
      `CONFIG_EFI_STUB=y` — the same as ours in every field that decides where
      a kernel may be loaded. The kernel this loader *does* boot is
      relocatable and linked for 16 MB, exactly like ours.
- [x] ~~Which needs per-board kernel fragments first~~ — **no longer needed
      for this.** Worth having eventually, but it is not the prerequisite for
      anything on this list any more.
- [ ] **Disassemble `big_linux_boot`.** This is the next step, and
      black-box bisection has gone as far as it can: six hypotheses eliminated
      and the failure unchanged. The symbol is in the loader's table,
      `cisco-loader-4.0.0i.efi` is extracted, and it is GRUB's legacy
      `loader/i386/linux.c` with `grub_relocator16_*` — so what it writes into
      `boot_params` and where it far-jumps is readable. The question to answer
      is how much of the real-mode setup it copies and to where: ours is
      20,480 bytes against the vendor's 15,872, and the NBI header sits at
      `0x94400`, only 1 KiB above the boot sector at `0x94000`.
- [ ] **Or sidestep it: the USB/EFI-stub path does not use `boot_params` at
      all.** Both kernels have `CONFIG_EFI_STUB=y`, and the firmware executes
      EFI applications — so that route tests "does our kernel run on this
      board" independently of the loader. See item 1b.

## 1b. A USB stick — the other way to reach the same question

`Boot0003 "EFI USB Device"` is enabled and is `[ 3 ]` in the TAB menu, and the
firmware executes EFI applications perfectly — it is how it starts its own
loader and shell. So a FAT stick with `\EFI\BOOT\BOOTX64.EFI`,
`\EFI\BOOT\initrd.img` and `\startup.nsh` boots with no boot-variable
change at all, and **the EFI stub path does not involve the loader's
`boot_params` at all** — which makes it an independent test of whether our
kernel runs on this board.

It needs somebody at the rack. Procedure in
[install.md](install.md#test-it-from-a-usb-stick-instead).

What either path settles:

- [ ] **Does our kernel execute on this board at all?** Still unanswered. The
      loader path dies at the handoff and the USB path has not been tried.
- [ ] **The fans** — the only place the unmanaged duty can be observed.
- [ ] **The platform i2c bus** — `tools/mki2cmap.sh --yaml`. ⚠ Partly answered
      already, for free: the loader's own `debug 3` output shows it selecting
      **two** muxes, `0x73` and `0x70`, and reading the board EEPROM at
      **`0x52`**, which is where `CardIndex = 11091` comes from. Three
      addresses the HAL needs, and `debug 3` is a way to watch platform i2c
      with no OS in the way.
- [ ] **The disk, read-only** — `cat /proc/partitions` should show the vendor's
      `sda1..sda6` intact.
- [ ] **The management port**, by MAC — see item 2.

## 2. ~~Which of the four PCH GbE ports is `mgmt0`~~ — answered

**`01:00.1`, `8086:0438`, MAC `b4:de:31:3f:a5:c0`, driver `igb`.** Read off the
running switch: it is the only one of the four with a driver bound.

And the premise of the original question was wrong. The two `8086:0436`
functions are **`DH8900CC Null Device`** — placeholders the PCH exposes, which
report an Ethernet class code (`0x020000`, which is what the EFI shell prints)
and are not ports. No driver claims them and none should; there is no missing
PCI ID and no kernel patch.

So `CONFIG_IGB=y` is sufficient, and it is in the fragment.

- [ ] One thing left, and it is a naming question rather than a driver one:
      `01:00.2` is a genuine second `8086:0438` that goes nowhere, and `igb`
      will bind it as well as the front panel. NX-OS leaves it unbound. Which
      kernel name each gets depends on probe order, so `network.conf` must not
      be written against a guessed name — confirm by MAC during item 1.

## 3. Generate the port map, before the install rather than after

`tools/mkportmap.sh` and `tools/mkpolarity.sh`, from one `config show` capture
off the vendor's SDK shell. The datapath will not start without the first, and
ports link and carry nothing without the second.

Take the capture **while the vendor OS is still installed**, because after an
install it is gone — and getting it back means putting NX-OS back on the box.
This is the one irreversible ordering constraint on the whole page.

⚠ **Reaching that shell needs `feature bash-shell`, which is currently off.**
It is a running-config setting and the lab unit lost it to a power cycle, so
`run bash` returns a syntax error rather than a permission error. Turn it back
on first.

Both generators have been run against the real capture and produce the right
shape — 54 port map entries, 54 PHY addresses, 48 copper PHYs, 6 cage PHYs, 18
per-core lane maps — so what is left is running them against the switch you are
installing on.

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

- [ ] **Map the mux `0x70` channels to devices.** There is a tool for this
      now: `tools/mki2cmap.sh --yaml`, run under a netboot (item 1) where the
      bus is idle and ours. The kernel carries the path — `I2C_I801`,
      `I2C_MUX_PCA954x`, `GPIOLIB` — and the image carries the `i2c*` applets.
      The alternative, watching `dmesg` on the vendor OS while touching each
      subsystem, only works by elimination, because successful mux selections
      are not logged and only the failures are. Without the channel map an i2c
      map in `board.yml` would be a guess that reads a device that is not
      there.
- [ ] **Identify the parts, rather than believing the address hints.** The
      sensor drivers ship as modules so this can be done by binding one and
      checking the reading against the vendor's own numbers — the lab chassis
      idles at ASIC 56 °C, D1 38 °C, D2 37 °C, D3 31 °C. A wrong part name is
      worse than none: the driver binds, a sensor appears, and it reports a
      number read from the wrong register.
- [ ] **Land the Linux-i2c HAL on `main`.** `platform_hal.i2c` and
      `internal/platformhal/i2cmap.go` were written for the Edgecore AS4610
      and are on `board/edgecore-as4610-54t`. This board wants the same shape,
      which makes two — and neither can declare an i2c map until it is on
      `main`. This is the blocker between having the map and acting on it.
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
