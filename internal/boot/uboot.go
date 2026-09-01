package boot

import (
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
)

func init() { register(uboot{}) }

// uboot produces a FIT image for boards booted by U-Boot.
//
// This is a genuinely different shape from the other backends. ONIE and Aboot
// are handed an envelope which they unpack and run; U-Boot is not an installer
// at all. It loads an image into memory -- from flash, or over TFTP during
// bring-up -- and boots it. So the artifact is not something that installs
// itself, it is something U-Boot can load.
//
// A FIT rather than a legacy uImage because a FIT carries the kernel, the
// initramfs and the device tree in one file with its own integrity checks.
// Older boards need a device tree that the kernel cannot supply for itself,
// and keeping the three together means they cannot be updated out of step.
type uboot struct{}

func (uboot) ID() string { return "uboot" }

// dtc because mkimage shells out to it to compile the FIT description.
func (uboot) Tools() []string { return []string{"mkimage", "dtc"} }

func (uboot) Describe() string {
	return "a FIT image loaded by U-Boot, over TFTP during bring-up or from flash"
}

// its is the FIT description mkimage compiles.
//
// Load and entry addresses are left to the board, because they are properties
// of where its RAM is: an address that works on one SoC puts the kernel on top
// of something important on another.
const itsTemplate = `/dts-v1/;
/ {
    description = "NOSaic %s for %s";
    #address-cells = <1>;
    images {
        kernel {
            description = "NOSaic kernel";
            data = /incbin/("%s");
            type = "kernel";
            arch = "%s";
            os = "linux";
            compression = "%s";
            load = <%s>;
            entry = <%s>;
            hash { algo = "%s"; };
        };
        ramdisk {
            description = "NOSaic initramfs";
            data = /incbin/("%s");
            type = "ramdisk";
            arch = "%s";
            os = "linux";
            compression = "none";
%s            hash { algo = "%s"; };
        };
%s    };
    configurations {
        default = "nosaic";
        nosaic {
            description = "NOSaic %s";
            kernel = "kernel";
            ramdisk = "ramdisk";
%s        };
    };
};
`

// The device tree image and the line that puts it in the configuration.
//
// Two separate substitutions because a board without a device tree must
// produce a FIT with no fdt node at all, rather than one naming a file that is
// not there -- mkimage compiles the description with dtc, and an /incbin/ of a
// missing file is a build failure rather than a smaller image.
const itsFDT = `        fdt {
            description = "NOSaic device tree";
            data = /incbin/("%s");
            type = "flat_dt";
            arch = "%s";
            compression = "none";
%s            hash { algo = "%s"; };
        };
`

func (u uboot) Wrap(img Image, outDir string, log io.Writer) (string, error) {
	if img.Kernel == "" || img.Initramfs == "" {
		return "", fmt.Errorf("uboot needs a kernel and an initramfs")
	}
	if img.UBootArch == "" {
		return "", fmt.Errorf("uboot needs the board's u_boot_arch (ppc, arm, arm64, x86)")
	}
	load, entry := img.UBootLoad, img.UBootEntry
	if load == "" || entry == "" {
		return "", fmt.Errorf("uboot needs the board's load and entry addresses: " +
			"they depend on where that board's RAM is, and a wrong one overwrites something")
	}

	// mkimage does not compile the FIT description itself, it shells out to
	// dtc -- and dtc is only a Recommends of u-boot-tools, so an image built
	// with --no-install-recommends has mkimage and not dtc. Without this
	// check the failure is mkimage reporting that it cannot open a temporary
	// file it never got as far as creating, which says nothing about why.
	for _, tool := range []string{"mkimage", "dtc"} {
		if _, err := exec.LookPath(tool); err != nil {
			return "", fmt.Errorf("uboot needs %s: install u-boot-tools and "+
				"device-tree-compiler (both are in builder/Dockerfile.build)", tool)
		}
	}

	fmt.Fprintf(log, "==> building the FIT image\n")
	work, err := os.MkdirTemp("", "nosaic-fit-")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(work)

	// Absolute paths, because mkimage resolves the /incbin/ directives in the
	// description relative to its own working directory, not to the file.
	kernel, err := filepath.Abs(img.Kernel)
	if err != nil {
		return "", err
	}
	initrd, err := filepath.Abs(img.Initramfs)
	if err != nil {
		return "", err
	}

	// sha256 is the right default and is not universal: U-Boot 2013.01 on the
	// AS5610 knows crc32, md5 and sha1, and a FIT hashed with anything else
	// loads, reports the configuration it found, and then says it cannot get
	// the kernel image -- which reads like a malformed image rather than an
	// unsupported digest.
	algo := img.FITHash
	if algo == "" {
		algo = "sha256"
	}

	// A kernel that is already a legacy uImage cannot go into a FIT as-is:
	// bootm would hand the kernel's own 64-byte header to the decompressor.
	// Unwrapping here rather than asking each arch to stage a second kernel
	// artifact keeps one kernel per build, and the header carries the
	// compression the kernel build actually chose.
	kernel, kcomp, err := unwrapUImage(kernel, work)
	if err != nil {
		return "", err
	}

	// A board with no device tree gets a FIT with no fdt node and a
	// configuration that names none, which is what an x86 board booted by
	// U-Boot wants. A board that supplies one gets both, and the kernel is
	// handed the tree by `bootm addr#nosaic`.
	fdtImage, fdtRef := "", ""
	if img.DTB != "" {
		dtb, err := filepath.Abs(img.DTB)
		if err != nil {
			return "", err
		}
		if _, err := os.Stat(dtb); err != nil {
			return "", fmt.Errorf("device tree %s: %w", img.DTB, err)
		}
		fdtImage = fmt.Sprintf(itsFDT, dtb, img.UBootArch, loadLine(img.FDTAddr), algo)
		fdtRef = "            fdt = \"fdt\";\n"
	}

	its := fmt.Sprintf(itsTemplate,
		img.Version, img.Board,
		kernel, img.UBootArch, kcomp, load, entry, algo,
		initrd, img.UBootArch, loadLine(img.RamdiskAddr), algo,
		fdtImage,
		img.Version,
		fdtRef)
	itsPath := filepath.Join(work, "nosaic.its")
	if err := os.WriteFile(itsPath, []byte(its), 0o644); err != nil {
		return "", err
	}

	out, err := filepath.Abs(filepath.Join(outDir,
		fmt.Sprintf("NOSaic-%s-%s.itb", img.Version, img.Board)))
	if err != nil {
		return "", err
	}
	cmd := exec.Command("mkimage", "-f", itsPath, out)
	cmd.Dir = work
	if b, err := cmd.CombinedOutput(); err != nil {
		return "", fmt.Errorf("mkimage: %v\n%s", err, b)
	}

	// U-Boot boards are brought up by typing commands at a console, so the
	// commands are part of the artifact rather than something to look up.
	// The image is staged somewhere other than where the kernel unpacks. A
	// board that has not said where gets its unpack address plus 32 MiB,
	// which is clear of a kernel of any plausible size and is what the boards
	// seen so far use anyway.
	stage := img.UBootStage
	if stage == "" {
		stage = "0x02000000"
	}

	notes := fmt.Sprintf(`# Booting NOSaic %s on %s with U-Boot
#
# Over the network, into RAM. Nothing is written to the board, so this is the
# way to try an image on a switch whose bootloader and rescue system share a
# disk with the OS -- installing there is what removes the way back.
#
# Set the addresses in RAM only. A saveenv during bring-up is how a board ends
# up unable to boot anything at all.
setenv ipaddr <board-ip>; setenv netmask <mask>; setenv gatewayip <gateway>
setenv serverip <tftp-server>
# or, if the segment has DHCP: setenv autoload no; dhcp; setenv serverip <tftp-server>

# bootargs must be set explicitly. Without it the kernel falls back to its
# built-in command line, and the failure is a board that loads the image and
# then prints nothing at all -- no panic, no console.
setenv bootargs 'console=%s'

# %s is where the image is parked; %s is where the kernel unpacks to.
# They must not be the same address.
tftpboot %s %s
bootm %s#nosaic

# Once it is known good, from flash. The offset is board-specific.
# Writing to flash is the point of no return on a board with one bank.
`, img.Version, img.Board, img.Console, stage, load,
		stage, filepath.Base(out), stage)

	notesPath := filepath.Join(outDir, "uboot-commands.txt")
	if err := os.WriteFile(notesPath, []byte(notes), 0o644); err != nil {
		return "", err
	}
	fmt.Fprintf(log, "    %s\n    %s\n", out, notesPath)
	return out, nil
}

// unwrapUImage strips a legacy U-Boot header, if there is one.
//
// Returns the path to use as FIT payload and the compression to declare. A
// file that is not a uImage is passed through untouched as "none", because
// that is what a raw kernel binary is.
func unwrapUImage(path, work string) (string, string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", "", err
	}
	defer f.Close()

	var hdr [64]byte
	if _, err := io.ReadFull(f, hdr[:]); err != nil {
		// Too short to be a uImage, so it is not one.
		return path, "none", nil
	}
	if binary.BigEndian.Uint32(hdr[0:4]) != 0x27051956 {
		return path, "none", nil
	}

	// Byte 31 is the compression field. Only the values a kernel build
	// actually produces are listed; anything else is refused rather than
	// guessed at, because declaring the wrong one in the FIT produces a board
	// that loads the image and then sits silent.
	comp, ok := map[byte]string{0: "none", 1: "gzip", 3: "lzma", 5: "lz4", 6: "zstd"}[hdr[31]]
	if !ok {
		return "", "", fmt.Errorf("%s is a uImage compressed in a way this backend does not know (%d)", path, hdr[31])
	}

	out := filepath.Join(work, "kernel.payload")
	dst, err := os.Create(out)
	if err != nil {
		return "", "", err
	}
	defer dst.Close()
	if _, err := io.Copy(dst, f); err != nil {
		return "", "", err
	}
	return out, comp, nil
}

// loadLine renders an optional load address for a FIT sub-image.
//
// Empty means no load property at all, which tells U-Boot to place the blob
// itself. That is the right default, but not always the right answer: U-Boot
// 2013.01 on some boards wants to be told, and a board that has found that out
// says so in its board.yml rather than carrying a patched builder.
func loadLine(addr string) string {
	if addr == "" {
		return ""
	}
	return fmt.Sprintf("            load = <%s>;\n", addr)
}
