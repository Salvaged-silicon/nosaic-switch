/* SPDX-License-Identifier: Apache-2.0 */
package imgbuild

import (
	"bytes"
	"encoding/binary"
	"fmt"
)

// An NBI container the Cisco NX-OS loader will accept.
//
// This exists because the Nexus 3172TQ's firmware cannot be told to boot
// anything. Its boot manager ignores UEFI BootOrder and BootNext -- BootNext
// survives a boot unconsumed, which is proof it is never read -- and always
// launches the vendor loader. The loader is therefore the only thing that runs
// on every boot, and an NBI is the only format it loads.
//
// Stock mknbi-linux emits five or six segments. This loader parses at most
// FOUR descriptors and reads the fifth from beyond the header it kept, so it
// reports a garbage load address and refuses. It also rejects load addresses
// below roughly 0x90000, and does its own Linux setup in rom_loader_linux_boot,
// so mknbi's real-mode stub is not wanted either. What it does accept is the
// shape Cisco's own images use:
//
//	vtag 17   bzImage[0:512]     -> 0x94000    the boot sector, exactly
//	vtag 20   bzImage[512:]      -> 0x100000   kernel, tail-padded
//	vtag 21   initrd (optional)  -> rdAddr
//
// Every constant below was measured against the vendor's own image and the
// loader's disassembly; the reasoning is in cisco-re's loader_linux_handoff.md
// and is summarised where it decides a value.
const (
	nbiMagic   = 0x1B031336
	nbiHdrLen  = 1024 // Cisco pads the header area to 1024 before segment data
	nbiSector  = 512
	nbiRdAddr  = 0x2000000
	nbiKernAt  = 0x100000
	nbiBootAt  = 0x94000
	nbiMaxSegs = 4
)

// nbiWrapperCmdline is what this boot path needs on top of whatever the board
// asked for, and belongs here rather than in board.yml because it is a
// property of the loader, not of the switch.
//
// nokaslr because physical KASLR decompresses over the initrd this loader
// placed: "Initramfs unpacking failed: invalid magic at start of compressed
// archive", the start having been overwritten.
//
// earlyprintk because without it the decompressor's console is VGA only, and
// every failure before the 8250 driver comes up is completely silent.
const nbiWrapperCmdline = "earlyprintk=serial,ttyS0,9600 nokaslr"

func nbiPad(n, a int) int { return (n + a - 1) / a * a }

// nbiSect zero-extends a segment's data to a whole number of sectors.
func nbiSect(b []byte) []byte {
	out := make([]byte, nbiPad(len(b), nbiSector))
	copy(out, b)
	return out
}

// nbiShim patches a 32-bit stub into the kernel to repair boot_params.
//
// Two things the loader gets wrong can only be fixed after it has finished
// writing boot_params and before the kernel reads it:
//
//	0x1c0  efi_loader_signature  it writes "EL64" but writes efi_systab with a
//	                             32-bit load and never writes efi_systab_hi, so
//	                             get_rsdp_addr() walks half a pointer before
//	                             printing anything.
//	0x228  cmd_line_ptr          it points this at a command line of its own
//	                             making, which we cannot influence -- so no
//	                             console=, no earlyprintk=, no root=.
//
// ⚠ Do NOT touch ramdisk_image (0x218) / ramdisk_size (0x21c). The movl $0x0
// pair in grub_load_linux looks like the loader refusing an initrd, but it runs
// for vtag 20; vtag 21 is dispatched afterwards to grub_load_initrd, which
// places the initrd where it wants and fills those fields in properly. Writing
// our own guess over the loader's correct value is what "Initramfs unpacking
// failed: invalid magic at start of compressed archive" looks like.
//
// The hook is the 32-bit entry itself: the loader jumps there with %esi holding
// boot_params, which is all the shim needs -- no symbols, no disassembly. The
// stub lives in the zero-filled alignment slack between the 32-bit lret and
// startup_64's mandatory 0x200 offset. Both that slack and the prologue it
// relocates are asserted, so a kernel that changes shape fails the build loudly
// instead of booting strangely.
func nbiShim(k []byte, cmdline string) ([]byte, error) {
	const padAt, padEnd = 0x12c, 0x200
	pm := (int(k[0x1f1]) + 1) * nbiSector

	// cld ; cli ; movl $__BOOT_DS,%eax -- the head of startup_32 as left by
	// recipes/linux/patches/0002-x86-boot-reload-the-data-segments-at-startup_32.patch.
	prologue := []byte{0xfc, 0xfa, 0xb8, 0x18, 0x00, 0x00, 0x00}
	if !bytes.Equal(k[pm:pm+len(prologue)], prologue) {
		return nil, fmt.Errorf("startup_32 does not begin as expected (%x); the shim "+
			"relocates those bytes and must not split an instruction", k[pm:pm+len(prologue)])
	}
	for _, b := range k[pm+padAt : pm+padEnd] {
		if b != 0 {
			return nil, fmt.Errorf("the %#x..%#x padding before startup_64 is not free; "+
				"nowhere to put the shim", padAt, padEnd)
		}
	}

	setField := func(off, val uint32) []byte { // movl $val,off(%esi)
		b := []byte{0xc7, 0x86}
		b = binary.LittleEndian.AppendUint32(b, off)
		return binary.LittleEndian.AppendUint32(b, val)
	}

	shim := setField(0x1c0, 0)
	var cmd []byte
	if cmdline != "" {
		cmd = append([]byte(cmdline), 0)
		// The string travels in the image, so the shim has to find out where
		// it itself landed: call the next instruction and pop the return
		// address. Everything else here is position-independent.
		//
		// `call .+0` pushes the address of the NEXT byte, which is the pop
		// itself -- so the pop counts towards the distance to the string.
		shim = append(shim, 0xe8, 0, 0, 0, 0, 0x58) // call .+0 ; pop %eax
		toStr := uint32(1 + 5 + 6 + len(prologue) + 5)
		shim = append(shim, 0x05)
		shim = binary.LittleEndian.AppendUint32(shim, toStr) // add $toStr,%eax
		shim = append(shim, 0x89, 0x86)
		shim = binary.LittleEndian.AppendUint32(shim, 0x228) // mov %eax,0x228(%esi)
	}
	shim = append(shim, prologue...)
	back := padAt + len(shim) + 5
	shim = append(shim, 0xe9)
	shim = binary.LittleEndian.AppendUint32(shim, uint32(int32(len(prologue)-back)))
	shim = append(shim, cmd...)

	if padAt+len(shim) > padEnd {
		return nil, fmt.Errorf("the shim is %d bytes and does not fit in the %#x padding",
			len(shim), padEnd-padAt)
	}

	out := make([]byte, len(k))
	copy(out, k)
	copy(out[pm+padAt:], shim)
	jmp := []byte{0xe9}
	jmp = binary.LittleEndian.AppendUint32(jmp, uint32(int32(padAt-5)))
	for len(jmp) < len(prologue) {
		jmp = append(jmp, 0x90) // nop
	}
	copy(out[pm:], jmp)
	return out, nil
}

type nbiSeg struct {
	vtag   byte
	load   uint32
	data   []byte
	memLen uint32
}

// buildNBI wraps a bzImage and an optional initrd into a container the loader
// accepts. cmdline is the board's own command line; the wrapper's additions are
// appended here.
func buildNBI(kernel, initrd []byte, cmdline string) ([]byte, error) {
	if len(kernel) < 0x268 || kernel[0x1fe] != 0x55 || kernel[0x1ff] != 0xaa ||
		!bytes.Equal(kernel[0x202:0x206], []byte("HdrS")) {
		return nil, fmt.Errorf("not a bzImage (no 0xAA55 / \"HdrS\")")
	}
	// init_size tells how much memory the kernel needs once decompressed.
	initSize := binary.LittleEndian.Uint32(kernel[0x260:])

	full := cmdline
	if full != "" {
		full += " "
	}
	full += nbiWrapperCmdline

	k, err := nbiShim(kernel, full)
	if err != nil {
		return nil, err
	}

	// ⚠ EXACTLY 512 BYTES, AND NO MORE. grub_load_linux reads this segment into
	// its own buffer and checks the boot signature at buf[0x1fe]. Enlarging the
	// segment moves what lands under that check, and the loader says so in hex:
	// 640 bytes gives "invalid magic number: 211"; 640 plus a 384-byte pad
	// gives "invalid magic number: 0".
	segs := []nbiSeg{{17, nbiBootAt, k[:nbiSector], 0x400}}

	// Pad the tail. grub_load_linux sites its params page and real-mode area by
	// walking DOWN from the top of the loaded kernel extent. With the segment
	// ending exactly at the end of the image, that carve-out lands in the last
	// 0x5000 bytes of the kernel's own .data -- where the decompressor keeps
	// live state. Three variables in that window gave three separate silent
	// resets: pgtable_l5_enabled, efi_is64, and the PTE mask. So give the
	// loader somewhere else to put it. 0x3000 past the real-mode reserve is the
	// tested figure; the minimum was not bisected.
	rmReserve := (int(k[0x1f1]) + 1) * nbiSector
	body := append(nbiSect(k[nbiSector:]), make([]byte, nbiPad(rmReserve+0x3000, nbiSector))...)
	memLen := uint32(nbiPad(len(body), nbiSector))
	if initSize > memLen {
		memLen = initSize
	}
	segs = append(segs, nbiSeg{20, nbiKernAt, body, memLen})

	if len(initrd) > 0 {
		// One sector of slack past the end. The loader accounts the header as
		// 512 bytes while segment data actually starts at 1024, so it stops
		// reading 512 bytes early and those bytes come off the END of the last
		// segment. On the initrd that is "Initramfs unpacking failed: read
		// error". Give the truncation something harmless to eat.
		rd := append(nbiSect(initrd), make([]byte, nbiSector)...)
		segs = append(segs, nbiSeg{21, nbiRdAddr, rd, uint32(nbiPad(len(rd), nbiSector))})
	}
	if len(segs) > nbiMaxSegs {
		return nil, fmt.Errorf("%d segments: the loader cannot parse more than %d",
			len(segs), nbiMaxSegs)
	}

	hdr := make([]byte, nbiHdrLen)
	binary.LittleEndian.PutUint32(hdr[0:], nbiMagic)
	hdr[4] = 0x54 // header length nibble + flags
	hdr[5] = 0x00
	binary.LittleEndian.PutUint16(hdr[6:], 0)           // vendortag
	binary.LittleEndian.PutUint32(hdr[8:], 0x94400000)  // header location 9440:0000
	binary.LittleEndian.PutUint32(hdr[12:], 0x92800000) // exec address    9280:0000
	copy(hdr[0x10:], "mknbi-linux-1.2-6")

	p := 0x24
	for i, s := range segs {
		hdr[p] = 0x04 // descriptor length / 16
		hdr[p+1] = s.vtag
		hdr[p+2] = 0
		// flags: bits 0-1 placement (0 = absolute), bit 2 = last segment.
		// Cisco's own images mark the final descriptor 0x04 and leave the
		// length byte at 0x04 throughout.
		if i == len(segs)-1 {
			hdr[p+3] = 0x04
		}
		binary.LittleEndian.PutUint32(hdr[p+4:], s.load)
		binary.LittleEndian.PutUint32(hdr[p+8:], uint32(len(s.data)))
		binary.LittleEndian.PutUint32(hdr[p+12:], s.memLen)
		p += 16
	}

	out := hdr
	for _, s := range segs {
		out = append(out, s.data...)
	}
	return out, nil
}
