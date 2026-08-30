package scd

import "github.com/salvaged-silicon/nosaic-switch/internal/platformhal/scdsmbus"

// The SCD's SMBus masters, reached through the GPL-2.0 scdsmbus package.
//
// That package is separate on purpose -- its transfer protocol is ported from
// Arista's GPL driver, where the register addresses used elsewhere in this
// package are not. Register access is handed to it rather than imported by it,
// so the dependency runs one way and the licence boundary is an import.

// Read32 exposes bounds-checked register access to the SMBus master.
func (s *SCD) Read32(off int) uint32 { return s.read32(off) }

// Write32 exposes bounds-checked register access to the SMBus master.
func (s *SCD) Write32(off int, v uint32) { s.write32(off, v) }

// Len is the size of the mapped BAR.
func (s *SCD) Len() int { return len(s.bar) }

func (s *SCD) smb() *scdsmbus.Master { return scdsmbus.New(s) }
