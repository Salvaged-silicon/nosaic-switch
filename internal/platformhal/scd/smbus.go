// SPDX-License-Identifier: GPL-2.0-only
//
// THIS FILE IS GPL-2.0, AND THE ONLY ONE IN NOSAIC THAT IS.
//
// The SMBus accelerator protocol below is ported from Arista's GPL-2.0
// scd-smbus.c (scd_smbus_master_xfer). Transcribing a driver's bitfield layout
// and transfer sequence produces a derivative work, so the licence follows it
// -- reading a register *map* is fact-gathering and does not, but this is more
// than a map. The rest of NOSaic is Apache-2.0; the boundary is this file.
//
// EdgeNOS reached the same conclusion about its own scdreset.c, and this port
// follows its implementation, which established the sequence on this hardware.

package scd

import (
	"fmt"
	"time"
)

// The SCD carries nine SMBus accelerators at 0x8000, stride 0x80. Everything
// on the board that is not on the PCI bus hangs off one of them: the thermal
// sensors, the fan controller, the power supplies.
const (
	smbStride  = 0x80
	smbBase0   = 0x8000
	smbReqOff  = 0x10
	smbCSOff   = 0x20
	smbRespOff = 0x30

	// Defaults from the driver's per-address bus_params. Inventing values here
	// happens to read some devices and is not what the reference does.
	smbParamT  = 1
	smbParamED = 0
)

func smbBase(accel int) int { return smbBase0 + smbStride*accel }

// smbReq packs one request word.
//
//	d:8 ss:6 ed:1 br:1 dat:2 t:2 sp:1 da:1 dod:1 st:1 bs:4 ti:4
func smbReq(d, ss, ed, br, t, sp, da, dod, st, bs, ti uint32) uint32 {
	return (d & 0xff) | (ss&0x3f)<<8 | (ed&1)<<14 | (br&1)<<15 |
		(t&3)<<18 | (sp&1)<<20 | (da&1)<<21 | (dod&1)<<22 |
		(st&1)<<23 | (bs&0xf)<<24 | (ti&0xf)<<28
}

func csNRS(v uint32) uint32 { return v & 0x3ff }
func csNRQ(v uint32) uint32 { return (v >> 16) & 0x3ff }
func csBRB(v uint32) uint32 { return (v >> 26) & 1 }
func csFE(v uint32) uint32  { return (v >> 30) & 1 }

const smbRst = 1 << 31

// smbEnter puts an accelerator into a state where a transfer can start.
func (s *SCD) smbEnter(base int) error {
	cs := s.read32(base + smbCSOff)

	// A reset left asserted must be released first, and it is not covered by
	// the busy test below: an idle-but-held master reads with every one of
	// those fields clear and only the reset bit set, so the test passes and
	// the master stays down.
	if cs&smbRst != 0 {
		s.write32(base+smbCSOff, cs&^uint32(smbRst))
		time.Sleep(time.Millisecond)
		cs = s.read32(base + smbCSOff)
	}
	if csFE(cs) == 0 && csBRB(cs) == 0 && csNRQ(cs) == 0 && csNRS(cs) == 0 {
		return nil
	}

	s.write32(base+smbCSOff, cs|smbRst)
	for d := time.Millisecond; d <= 8*time.Millisecond; d *= 2 {
		time.Sleep(d)
		cs = s.read32(base + smbCSOff)
		if csFE(cs) == 0 && csBRB(cs) == 0 && csNRQ(cs) == 0 && csNRS(cs) == 0 {
			// Release it. Leaving the reset asserted holds the master down and
			// every later transfer on that accelerator reports no response.
			s.write32(base+smbCSOff, cs&^uint32(smbRst))
			return nil
		}
	}
	return fmt.Errorf("SMBus accelerator would not reset, cs=%#08x", cs)
}

// SMBusReadByte reads one register from one device.
//
// An SMBus read-byte-data is two i2c messages -- write the register index,
// then read a byte -- so the transfer size is four words and the accelerator
// is fed four requests. The last response carries the data.
func (s *SCD) SMBusReadByte(accel, bus, addr, reg int) (byte, error) {
	base := smbBase(accel)
	if base+smbRespOff+4 > len(s.bar) {
		return 0, fmt.Errorf("SMBus accelerator %d is past the mapped BAR", accel)
	}
	if err := s.smbEnter(base); err != nil {
		return 0, err
	}

	const ss = 4
	b, ti := uint32(bus), uint32(0)
	next := func() uint32 { v := ti; ti++; return v }

	// msg 0: write the register index
	s.write32(base+smbReqOff, smbReq(uint32(addr)<<1, ss, 0, 0, smbParamT, 0, 0, 1, 1, b, next()))
	s.write32(base+smbReqOff, smbReq(uint32(reg), 0, smbParamED, 0, smbParamT, 0, 0, 1, 0, b, next()))
	// msg 1: read one byte back. sp=1 marks the last word.
	s.write32(base+smbReqOff, smbReq(uint32(addr)<<1|1, 0, 0, 0, smbParamT, 0, 0, 1, 1, b, next()))
	s.write32(base+smbReqOff, smbReq(0, 0, smbParamED, 0, smbParamT, 1, 0, 0, 0, b, next()))

	var cs uint32
	for i := 0; i < 200; i++ {
		cs = s.read32(base + smbCSOff)
		if csNRS(cs) >= ss {
			break
		}
		time.Sleep(time.Millisecond)
	}
	cs = s.read32(base + smbCSOff)
	if csNRS(cs) == 0 {
		return 0, fmt.Errorf("SMBus a%d b%d %#02x reg %#02x: no response, cs=%#08x",
			accel, bus, addr, reg, cs)
	}

	var resp uint32
	for i := uint32(0); i < csNRS(cs); i++ {
		resp = s.read32(base + smbRespOff)
	}
	switch {
	case resp>>10&1 != 0:
		return 0, fmt.Errorf("SMBus %#02x reg %#02x: no acknowledgement (nothing at that address)", addr, reg)
	case resp>>9&1 != 0:
		return 0, fmt.Errorf("SMBus %#02x reg %#02x: timeout", addr, reg)
	case resp>>8&1 != 0:
		return 0, fmt.Errorf("SMBus %#02x reg %#02x: bus error", addr, reg)
	}
	return byte(resp & 0xff), nil
}

// SMBusWriteByte writes one register on one device.
//
// Three request words rather than four: address and write bit, register
// index, data. There is no read phase and no repeated start.
func (s *SCD) SMBusWriteByte(accel, bus, addr, reg int, val byte) error {
	base := smbBase(accel)
	if base+smbRespOff+4 > len(s.bar) {
		return fmt.Errorf("SMBus accelerator %d is past the mapped BAR", accel)
	}
	if err := s.smbEnter(base); err != nil {
		return err
	}

	const ss = 3
	b := uint32(bus)
	s.write32(base+smbReqOff, smbReq(uint32(addr)<<1, ss, 0, 0, smbParamT, 0, 0, 1, 1, b, 0))
	s.write32(base+smbReqOff, smbReq(uint32(reg)&0xff, 0, 0, 0, smbParamT, 0, 0, 1, 0, b, 1))
	s.write32(base+smbReqOff, smbReq(uint32(val), 0, smbParamED, 0, smbParamT, 1, 0, 1, 0, b, 2))

	var cs uint32
	for i := 0; i < 200; i++ {
		cs = s.read32(base + smbCSOff)
		if csNRS(cs) >= ss {
			break
		}
		time.Sleep(time.Millisecond)
	}
	cs = s.read32(base + smbCSOff)
	if csNRS(cs) == 0 {
		return fmt.Errorf("SMBus a%d b%d %#02x reg %#02x: no response to write, cs=%#08x",
			accel, bus, addr, reg, cs)
	}
	for i := uint32(0); i < csNRS(cs) && i < 8; i++ {
		resp := s.read32(base + smbRespOff)
		if resp>>8&1 != 0 || resp>>9&1 != 0 || resp>>10&1 != 0 {
			return fmt.Errorf("SMBus %#02x reg %#02x: write not acknowledged", addr, reg)
		}
	}
	return nil
}
