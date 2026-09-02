# What EdgeNOS already learned about this chip

EdgeNOS attempted the same thing NOSaic is doing on this board -- the full
OpenBCM SDK over a user-mode BDE on a BCM56846 -- and wrote it down in
`edgenos/docs/full-sdk-port-5610.md`. Read that before debugging anything here.
This file records what carries over, what does not, and where the two trees now
disagree, because in one place NOSaic is ahead and it would be easy to
reintroduce their workaround by mistake.

None of it is code. EdgeNOS drives the chip through CDK/BMD (`bmd_vlan_port_add`,
`bmd_port_stp_set`) and its SDK work is against 6.5.16 with a patched source
tree; NOSaic uses unpatched 6.5.24 through `bcm_*`. The facts port; the calls do
not.

## Taken

**Per-port service VLANs.** Every front-panel port alone in VLAN 3300+port,
untagged, CPU tagged, VLAN 1 emptied. Cumulus's layout, reproduced in
`vlan_init_resv_per_port()`. This is the board's resting state and the reason a
switch here does not bridge its own uplinks together. Adopted; see the README.

**The front panel.** TX_DISABLE on PCA9506 expanders, the QSFP control expander
at 0x23 that must go high rather than low, and the DS100DF410 retimer sequence
whose CDR reset is not optional. Adopted in `scripts/sfp-init.sh`.

## Not taken, and why

EdgeNOS's `config.bcm` lists knobs as required that NOSaic does not set. They
were right for their situation and are not needed here -- our bring-up logs zero
warnings and zero errors -- so they are recorded as history rather than omitted
by oversight:

| knob | theirs | ours |
|---|---|---|
| `soc_skip_reset=1` | their chip was already initialised by edged | we attach a cold chip and let the SDK reset it |
| `mem_clear_hw_acceleration=0` | "DMA clears silently fail here" | **see below** |
| `skip_ipmc_init=1` | ipmc_init hit "Table full" and rolled back the unit | ipmc_init passes |
| `phy_null=1`, `bcm_linkscan_interval=0` | PHYs driven outside the SDK | we want the SDK's link scan |
| `parity_enable=0`, `mem_scan_enable=0` | scrubber cost/noise | re-enabled once DMA worked |

## Where NOSaic is ahead: the DMA failure has a cause, not a workaround

`mem_clear_hw_acceleration=0` is described there as CRITICAL, because "DMA
clears silently fail here". That is the same failure NOSaic spent days on, and
it is not a property of this chip. Every DMA the chip issued was correct and was
discarded one hop upstream, at the P2020 PCIe root port, whose Bus Master Enable
was clear because Linux binds no driver to either end and so calls
`pci_set_master()` on neither.

With the bridge enabled (`datapath/tdp/bde.c:enable_upstream_mastering`), table
DMA, TSLAM DMA, counter DMA and packet DMA all work, and full bring-up takes 13
seconds. Do not port that knob across. If DMA appears to fail here again, check
every bridge between the chip and memory before disabling anything.

## Ahead of us, not yet our problem

Their open blocker is that an IFP ACL sits correctly in the TCAM and never
matches live traffic -- proven not to be the TCAM contents, the selcodes, the
slice map or the enables. Their recommended next step is a register diff against
a live Cumulus that does drop correctly. NOSaic has not reached ACLs, and when
it does, that is the first thing to read.

## One to watch: endianness

For 6.5.16 they patched `soc_endian_config` to force `CMIC_ENDIAN_SELECT` to
`0x04000004` -- big-endian DMA, **PIO byte swapping off** -- because the default
`0x05000005` byte-swapped the S-Channel message buffer. NOSaic sets
`0x05000005`, reads it back as `0x05050505`, and S-Channel completes in two
polls with correct data, so 6.5.24 evidently does not have the same defect. It
is recorded because if S-Channel ever returns plausible-but-wrong data here,
this is the first thing to suspect and it costs one register to test.
