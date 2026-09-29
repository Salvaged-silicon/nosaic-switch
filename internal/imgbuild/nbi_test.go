/* SPDX-License-Identifier: Apache-2.0 */
package imgbuild

import (
	"bytes"
	"encoding/binary"
	"testing"
)

// A bzImage-shaped stub: enough header for the builder to read, a plausible
// startup_32 prologue at the protected-mode base, and free padding after it.
func fakeBzImage(t *testing.T) []byte {
	t.Helper()
	const setupSects = 4
	pm := (setupSects + 1) * nbiSector
	k := make([]byte, pm+0x1000)
	k[0x1f1] = setupSects
	k[0x1fe], k[0x1ff] = 0x55, 0xaa
	copy(k[0x202:], "HdrS")
	binary.LittleEndian.PutUint32(k[0x260:], 0x800000) // init_size
	copy(k[pm:], []byte{0xfc, 0xfa, 0xb8, 0x18, 0x00, 0x00, 0x00})
	return k
}

func TestNBIRefusesSomethingThatIsNotABzImage(t *testing.T) {
	if _, err := buildNBI(make([]byte, 4096), nil, ""); err == nil {
		t.Error("a buffer with no boot signature was accepted as a kernel")
	}
}

// The loader parses at most four descriptors and reads a fifth from beyond the
// header it kept, so it reports a garbage load address rather than an error
// anyone could act on.
func TestNBIHasTheShapeTheLoaderParses(t *testing.T) {
	out, err := buildNBI(fakeBzImage(t), []byte("initrd"), "console=ttyS0,9600n8")
	if err != nil {
		t.Fatal(err)
	}
	if got := binary.LittleEndian.Uint32(out); got != nbiMagic {
		t.Errorf("magic is %#x, want %#x", got, nbiMagic)
	}
	if !bytes.HasPrefix(out[0x10:], []byte("mknbi-linux-1.2-6")) {
		t.Error("the mknbi tag the loader looks for is missing")
	}
	for i, want := range []byte{17, 20, 21} {
		if got := out[0x24+i*16+1]; got != want {
			t.Errorf("descriptor %d is vtag %d, want %d", i, got, want)
		}
	}
	// Bit 2 of the flags byte marks the final descriptor, and only it.
	for i, want := range []byte{0x00, 0x00, 0x04} {
		if got := out[0x24+i*16+3]; got != want {
			t.Errorf("descriptor %d flags %#x, want %#x", i, got, want)
		}
	}
}

// ⚠ EXACTLY 512 BYTES. grub_load_linux reads this segment into its own buffer
// and checks the boot signature at buf[0x1fe]; a longer segment moves what
// lands under that check and the loader refuses with "invalid magic number".
func TestTheBootSectorSegmentIsExactlyOneSector(t *testing.T) {
	out, err := buildNBI(fakeBzImage(t), nil, "")
	if err != nil {
		t.Fatal(err)
	}
	if n := binary.LittleEndian.Uint32(out[0x24+8:]); n != nbiSector {
		t.Errorf("the vtag 17 segment is %d bytes, want %d", n, nbiSector)
	}
	if out[nbiHdrLen+0x1fe] != 0x55 || out[nbiHdrLen+0x1ff] != 0xaa {
		t.Error("the boot signature is not where the loader reads it")
	}
}

// The command line only reaches the kernel through the shim: the loader points
// cmd_line_ptr at one of its own making, which we cannot influence.
func TestTheCommandLineTravelsInTheImage(t *testing.T) {
	out, err := buildNBI(fakeBzImage(t), nil, "console=ttyS0,9600n8 memmap=64M$0xb0000000")
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"console=ttyS0,9600n8", "memmap=64M$0xb0000000", nbiWrapperCmdline} {
		if !bytes.Contains(out, []byte(want)) {
			t.Errorf("%q is not in the image, so the kernel will never see it", want)
		}
	}
}

// A kernel whose shape has moved must fail the build rather than boot strangely:
// the shim relocates the prologue and must not split an instruction.
func TestAKernelWithAnUnexpectedEntryIsRefused(t *testing.T) {
	k := fakeBzImage(t)
	pm := (int(k[0x1f1]) + 1) * nbiSector
	k[pm] = 0x90 // not the prologue the shim relocates
	if _, err := buildNBI(k, nil, ""); err == nil {
		t.Error("a kernel whose startup_32 has changed was accepted")
	}

	k = fakeBzImage(t)
	k[pm+0x140] = 0x01 // something already living in the shim's padding
	if _, err := buildNBI(k, nil, ""); err == nil {
		t.Error("a kernel with no free padding was accepted")
	}
}
