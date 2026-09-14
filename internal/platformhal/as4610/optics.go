package as4610

import (
	"fmt"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// CageCount is how many front-panel transceiver cages the board has.
//
// Six on this switch: four SFP+ uplinks and two stacking ports. The 48 copper
// ports have no cages at all, which is the first time in this tree that the
// front panel and the cage list are different lengths — so nothing here may
// assume cage N is port N.
func (h *HAL) CageCount() int {
	if h.m.Cages == nil {
		return 0
	}
	return h.m.Cages.Count
}

// ReadModuleBytes returns n bytes from the module in cage, at i2c address
// addr, starting at offset.
//
// The smallest thing platformhal.Optics asks for, and deliberately so:
// everything above it — which pages to read, which of SFF-8472 and SFF-8636
// applies, how a 16-bit field becomes microwatts — is the same on every switch
// and lives in platformhal.ReadModule and package sff.
//
// ⚠ page IS REFUSED HERE, AND THAT IS A REAL LIMIT RATHER THAN AN OMISSION.
//
// Selecting an SFF page is a write to byte 127 of the module. On this board
// the six cages sit behind one mux and all answer at the same address, so a
// page left selected on one module is a page selected on whichever module the
// mux next connects — and a subsequent read of another cage returns its upper
// page instead of its lower one, which decodes to plausible numbers rather
// than to an error.
//
// Doing it safely needs the page select and the read to be one uninterrupted
// transaction, and the kernel's mux does not offer that across a channel
// switch. Until it does, this board reports light levels and the identity
// bytes that live in the always-mapped lower half, and refuses the rest by
// name. ReadModule already treats the upper page as best-effort, so an SFP
// still reports everything and a QSFP loses its vendor strings.
func (h *HAL) ReadModuleBytes(cage, addr, page, offset, n int) ([]byte, error) {
	if h.m.Cages == nil {
		return nil, fmt.Errorf("%w: this board declares no transceiver cages",
			platformhal.ErrUnsupported)
	}
	if cage < 1 || cage > h.m.Cages.Count {
		return nil, fmt.Errorf("cage %d: this board has %d", cage, h.m.Cages.Count)
	}
	if page >= 0 {
		return nil, fmt.Errorf("%w: selecting SFF page %d needs a write, and six "+
			"cages behind one mux all answer at %#02x — a page left selected on "+
			"one is selected on the next", platformhal.ErrUnsupported, page,
			h.m.Cages.CageAddr())
	}
	if n <= 0 || n > 256 {
		return nil, fmt.Errorf("cage %d: asked for %d bytes", cage, n)
	}

	bus := h.m.Cages.FirstBus + cage - 1
	b, err := h.busFor(bus)
	if err != nil {
		return nil, err
	}
	out, err := b.ReadAt(addr, offset, n)
	if err != nil {
		// An empty cage is the common case and is not a fault. The bus does
		// not distinguish it from a broken one, so neither does this — but it
		// says which cage and which bus, which is what turns "no module" into
		// something checkable.
		return nil, fmt.Errorf("cage %d (i2c-%d, %#02x): %w", cage, bus, addr, err)
	}
	return out, nil
}
