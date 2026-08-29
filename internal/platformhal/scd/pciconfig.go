package scd

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
)

// PCI configuration space offsets and the COMMAND bits used here.
const (
	pciCommand = 0x04
	// pciCmdMemorySpace lets the device decode memory cycles. Without it
	// every MMIO read returns 0xffffffff and every write is discarded.
	pciCmdMemorySpace = 1 << 1
	// pciCmdBusMaster lets the device initiate transfers of its own. Not set
	// here: nothing does DMA yet, and granting a chip that has not been
	// initialised the ability to write host memory is not a default worth
	// having. The datapath will set it when it needs it.
	pciCmdBusMaster = 1 << 2
)

// enableMemorySpace turns on memory decoding for a device.
//
// A device brought onto the bus by `echo 1 > /sys/bus/pci/rescan` is
// enumerated and has its BARs assigned, but nothing calls pci_enable_device on
// it -- that happens when a driver binds, and no driver binds to the switch
// chip. Its COMMAND register therefore stays 0x0000, and the symptom is exact
// and misleading: the device is present in lspci with a valid BAR, and every
// read of that BAR returns 0xffffffff, which is indistinguishable from a chip
// that is broken or still in reset.
//
// For comparison, the management NIC on this board reads 0x0406 -- memory
// space, bus master, INTx disabled -- because tg3 binds to it and enables it.
func enableMemorySpace(pciAddr string) (before, after uint16, err error) {
	path := filepath.Join("/sys/bus/pci/devices", pciAddr, "config")
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return 0, 0, fmt.Errorf("opening PCI config for %s: %w", pciAddr, err)
	}
	defer f.Close()

	buf := make([]byte, 2)
	if _, err := f.ReadAt(buf, pciCommand); err != nil {
		return 0, 0, fmt.Errorf("reading the COMMAND register of %s: %w", pciAddr, err)
	}
	before = binary.LittleEndian.Uint16(buf)
	if before&pciCmdMemorySpace != 0 {
		return before, before, nil
	}

	binary.LittleEndian.PutUint16(buf, before|pciCmdMemorySpace)
	if _, err := f.WriteAt(buf, pciCommand); err != nil {
		return before, 0, fmt.Errorf("enabling memory space on %s: %w", pciAddr, err)
	}

	// Read back. A config write that does not stick is a real possibility on
	// a device that has just come out of reset, and the failure it produces
	// downstream -- all-ones MMIO -- says nothing about its cause.
	if _, err := f.ReadAt(buf, pciCommand); err != nil {
		return before, 0, err
	}
	after = binary.LittleEndian.Uint16(buf)
	if after&pciCmdMemorySpace == 0 {
		return before, after, fmt.Errorf("memory space did not enable on %s: "+
			"COMMAND reads %#04x after writing %#04x", pciAddr, after, before|pciCmdMemorySpace)
	}
	return before, after, nil
}
