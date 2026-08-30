// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (C) the NOSaic authors.
//
// THIS PACKAGE IS GPL-2.0. THE REST OF NOSAIC IS APACHE-2.0.
//
// The SMBus accelerator protocol here is ported from Arista's GPL-2.0
// scd-smbus.c (scd_smbus_master_xfer). Transcribing a driver's bitfield layout
// and transfer sequence produces a derivative work, so the licence follows it.
// Reading a register *map* is fact-gathering and does not, which is why the
// rest of the platform HAL is unaffected.
//
// It lives in its own package so the licence boundary is a package boundary
// rather than a comment somebody has to notice. See LICENSE and README.md
// beside this file.
//
// It knows nothing about NOSaic: register access is injected, so this package
// depends on no other part of the tree and can be lifted out whole.

package scdsmbus

import (
	"fmt"
	"time"
)

// Registers is memory-mapped access to the device this master lives on.
//
// Injected rather than imported so this package depends on nothing else. The
// caller owns the mapping and the bounds checking.
type Registers interface {
	Read32(off int) uint32
	Write32(off int, v uint32)
	// Len is the size of the mapping, so a transfer can refuse to address
	// past it rather than reading whatever follows.
	Len() int
}

// Master is one SMBus accelerator block.
type Master struct{ r Registers }

// New returns a master over the given register access.
func New(r Registers) *Master { return &Master{r} }

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
func (m *Master) smbEnter(base int) error {
	cs := m.r.Read32(base + smbCSOff)

	// A reset left asserted must be released first, and it is not covered by
	// the busy test below: an idle-but-held master reads with every one of
	// those fields clear and only the reset bit set, so the test passes and
	// the master stays down.
	if cs&smbRst != 0 {
		m.r.Write32(base+smbCSOff, cs&^uint32(smbRst))
		time.Sleep(time.Millisecond)
		cs = m.r.Read32(base + smbCSOff)
	}
	if csFE(cs) == 0 && csBRB(cs) == 0 && csNRQ(cs) == 0 && csNRS(cs) == 0 {
		return nil
	}

	m.r.Write32(base+smbCSOff, cs|smbRst)
	for d := time.Millisecond; d <= 8*time.Millisecond; d *= 2 {
		time.Sleep(d)
		cs = m.r.Read32(base + smbCSOff)
		if csFE(cs) == 0 && csBRB(cs) == 0 && csNRQ(cs) == 0 && csNRS(cs) == 0 {
			// Release it. Leaving the reset asserted holds the master down and
			// every later transfer on that accelerator reports no response.
			m.r.Write32(base+smbCSOff, cs&^uint32(smbRst))
			return nil
		}
	}
	return fmt.Errorf("SMBus accelerator would not reset, cs=%#08x", cs)
}

// An SMBus read-byte-data is two i2c messages -- write the register index,
// then read a byte -- so the transfer size is four words and the accelerator
// is fed four requests. The last response carries the data.
// ReadReg reads one register from one device.
func (m *Master) ReadReg(accel, bus, addr, reg int) (byte, error) {
	base := smbBase(accel)
	if base+smbRespOff+4 > m.r.Len() {
		return 0, fmt.Errorf("SMBus accelerator %d is past the mapped BAR", accel)
	}
	if err := m.smbEnter(base); err != nil {
		return 0, err
	}

	const ss = 4
	b, ti := uint32(bus), uint32(0)
	next := func() uint32 { v := ti; ti++; return v }

	// msg 0: write the register index
	m.r.Write32(base+smbReqOff, smbReq(uint32(addr)<<1, ss, 0, 0, smbParamT, 0, 0, 1, 1, b, next()))
	m.r.Write32(base+smbReqOff, smbReq(uint32(reg), 0, smbParamED, 0, smbParamT, 0, 0, 1, 0, b, next()))
	// msg 1: read one byte back. sp=1 marks the last word.
	m.r.Write32(base+smbReqOff, smbReq(uint32(addr)<<1|1, 0, 0, 0, smbParamT, 0, 0, 1, 1, b, next()))
	m.r.Write32(base+smbReqOff, smbReq(0, 0, smbParamED, 0, smbParamT, 1, 0, 0, 0, b, next()))

	var cs uint32
	for i := 0; i < 200; i++ {
		cs = m.r.Read32(base + smbCSOff)
		if csNRS(cs) >= ss {
			break
		}
		time.Sleep(time.Millisecond)
	}
	cs = m.r.Read32(base + smbCSOff)
	if csNRS(cs) == 0 {
		return 0, fmt.Errorf("SMBus a%d b%d %#02x reg %#02x: no response, cs=%#08x",
			accel, bus, addr, reg, cs)
	}

	var resp uint32
	for i := uint32(0); i < csNRS(cs); i++ {
		resp = m.r.Read32(base + smbRespOff)
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

// Three request words rather than four: address and write bit, register
// index, data. There is no read phase and no repeated start.
// WriteReg writes one register on one device.
func (m *Master) WriteReg(accel, bus, addr, reg int, val byte) error {
	base := smbBase(accel)
	if base+smbRespOff+4 > m.r.Len() {
		return fmt.Errorf("SMBus accelerator %d is past the mapped BAR", accel)
	}
	if err := m.smbEnter(base); err != nil {
		return err
	}

	const ss = 3
	b := uint32(bus)
	m.r.Write32(base+smbReqOff, smbReq(uint32(addr)<<1, ss, 0, 0, smbParamT, 0, 0, 1, 1, b, 0))
	m.r.Write32(base+smbReqOff, smbReq(uint32(reg)&0xff, 0, 0, 0, smbParamT, 0, 0, 1, 0, b, 1))
	m.r.Write32(base+smbReqOff, smbReq(uint32(val), 0, smbParamED, 0, smbParamT, 1, 0, 1, 0, b, 2))

	var cs uint32
	for i := 0; i < 200; i++ {
		cs = m.r.Read32(base + smbCSOff)
		if csNRS(cs) >= ss {
			break
		}
		time.Sleep(time.Millisecond)
	}
	cs = m.r.Read32(base + smbCSOff)
	if csNRS(cs) == 0 {
		return fmt.Errorf("SMBus a%d b%d %#02x reg %#02x: no response to write, cs=%#08x",
			accel, bus, addr, reg, cs)
	}
	for i := uint32(0); i < csNRS(cs) && i < 8; i++ {
		resp := m.r.Read32(base + smbRespOff)
		if resp>>8&1 != 0 || resp>>9&1 != 0 || resp>>10&1 != 0 {
			return fmt.Errorf("SMBus %#02x reg %#02x: write not acknowledged", addr, reg)
		}
	}
	return nil
}
