/* SPDX-License-Identifier: Apache-2.0 */
package n3172tq

import "testing"

func chn(n int) *int { return &n }

func good() *KernelI2C {
	return &KernelI2C{
		Adapter: "SMBus I801 adapter",
		Mux:     &I2CDevice{Driver: "pca9548", Address: 0x70},
		Devices: []I2CDevice{{Driver: "adt7462", Address: 0x58, Channel: chn(0)}},
	}
}

func TestI2CAcceptsARealBoard(t *testing.T) {
	if err := good().Validate(); err != nil {
		t.Fatalf("a real board was rejected: %v", err)
	}
	var none *KernelI2C
	if err := none.Validate(); err != nil {
		t.Fatalf("a board with no i2c block was rejected: %v", err)
	}
}

// The addresses the i2c specification reserves. new_device accepts them, the
// driver binds to nothing, and the board comes up with no sensors and no error
// -- so this is the only place it can be caught.
func TestI2CRejectsUnaddressableAddresses(t *testing.T) {
	for _, addr := range []int{0x00, 0x02, 0x78, 0x80, -1} {
		k := good()
		k.Devices[0].Address = addr
		if err := k.Validate(); err == nil {
			t.Errorf("address %#x was accepted", addr)
		}
	}
}

// A channel with no mux is a board description that cannot be applied: there
// is no channel-N to resolve, so the part is silently never instantiated.
func TestI2CRejectsChannelWithoutMux(t *testing.T) {
	k := good()
	k.Mux = nil
	if err := k.Validate(); err == nil {
		t.Error("a channel with no mux was accepted")
	}
}

// The mux is what makes channels, so it cannot be behind one. Getting this
// wrong is how a board ends up describing a mux that can never be reached.
func TestI2CRejectsMuxBehindAChannel(t *testing.T) {
	k := good()
	k.Mux.Channel = chn(1)
	if err := k.Validate(); err == nil {
		t.Error("a mux behind a channel was accepted")
	}
}

// Two parts at one address on one bus cannot both exist, and the second
// new_device fails during boot rather than during review.
func TestI2CRejectsDuplicateAddressOnOneBus(t *testing.T) {
	k := good()
	k.Devices = append(k.Devices, I2CDevice{Driver: "lm75", Address: 0x58, Channel: chn(0)})
	if err := k.Validate(); err == nil {
		t.Error("two parts at 0x58 on one channel were accepted")
	}
	// The same address on a different channel is a different bus, and on this
	// board is exactly right: the two supplies are both 0x5b.
	k = good()
	k.Devices = []I2CDevice{
		{Driver: "pmbus", Address: 0x5b, Channel: chn(2)},
		{Driver: "pmbus", Address: 0x5b, Channel: chn(3)},
	}
	if err := k.Validate(); err != nil {
		t.Errorf("the two supplies at 0x5b on different channels were rejected: %v", err)
	}
}

// The adapter is matched by name; without one there is nothing to match and the
// script would fall back to a number, which is the bug it exists to avoid.
func TestI2CRequiresAnAdapterName(t *testing.T) {
	k := good()
	k.Adapter = ""
	if err := k.Validate(); err == nil {
		t.Error("a device list with no adapter name was accepted")
	}
}

func TestI2CRejectsNonKernelDriverNames(t *testing.T) {
	for _, d := range []string{"", "ADT7462", "adt 7462", "../etc/passwd", "adt7462;reboot"} {
		k := good()
		k.Devices[0].Driver = d
		if err := k.Validate(); err == nil {
			t.Errorf("driver name %q was accepted", d)
		}
	}
}
