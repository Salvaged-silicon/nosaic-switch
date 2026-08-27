package boot

import (
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
            compression = "none";
            load = <%s>;
            entry = <%s>;
            hash { algo = "sha256"; };
        };
        ramdisk {
            description = "NOSaic initramfs";
            data = /incbin/("%s");
            type = "ramdisk";
            arch = "%s";
            os = "linux";
            compression = "gzip";
            hash { algo = "sha256"; };
        };
    };
    configurations {
        default = "nosaic";
        nosaic {
            description = "NOSaic %s";
            kernel = "kernel";
            ramdisk = "ramdisk";
        };
    };
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

	its := fmt.Sprintf(itsTemplate,
		img.Version, img.Board,
		kernel, img.UBootArch, load, entry,
		initrd, img.UBootArch,
		img.Version)
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
	notes := fmt.Sprintf(`# Booting NOSaic %s on %s with U-Boot
#
# Over the network, which is how a board is brought up before anything has
# been written to its flash:
setenv ipaddr <board-ip>; setenv serverip <tftp-server>
tftpboot %s %s
bootm %s

# Once it is known good, from flash. The offset is board-specific.
# Writing to flash is the point of no return on a board with one bank.
`, img.Version, img.Board, load, filepath.Base(out), load)

	notesPath := filepath.Join(outDir, "uboot-commands.txt")
	if err := os.WriteFile(notesPath, []byte(notes), 0o644); err != nil {
		return "", err
	}
	fmt.Fprintf(log, "    %s\n    %s\n", out, notesPath)
	return out, nil
}
