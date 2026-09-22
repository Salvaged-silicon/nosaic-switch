# What EdgeNOS already learned about this chip

EdgeNOS ran on this exact chassis and got the FM6000 to forward packets. That is
further than any other prior art referenced in this tree, and it is the reason
this board's todo list starts from a different place than a blank one would.

None of it is code. The investigation lives in its own repository and stays
there, per [CONTRIBUTING](../../../CONTRIBUTING.md). **The facts port; the
method does not** — and on this board that distinction is the whole design,
because the method is what NOSaic has ruled out.

## What it achieved

From a cold chip, in about four minutes: clocks and boot control, BIST, the
scheduler, the SBus master and SerDes SPICO, 129 memory fills by direct MMIO,
microcode in two parts, a port-and-forwarding bring-up, laser enable, and then
frames out and in — verified at the far end by an independent switch rather than
by the box's own counters.

That last detail is worth copying as a habit. A switch reporting its own
transmit counters is not evidence that anything left the building.

## Taken

**The chip off-buses on uninitialised bank memory.** Reading or writing an
ECC-uninitialised word in `STATS`, `MCAST_MID` or `MCAST_POST` escalates to a
chip-fatal and the endpoint leaves the PCIe bus — config space and BAR0 both
`0xffffffff`, link still up. From the host it looks like a hang, an RCU stall or
a reboot, never an error. **This is the single most expensive fact about this
chip** and it is why an off-bus detector is the first thing written here, before
any bring-up.

**`EPL_CFG_B` at `0xE3B02` selects the PCS type.** `0x00090003` is
`Port0PcsSel=3`, 10GBASE-R; a cold chip reads `0x00080000`. It never appears in
a port-bounce trace because EOS sets it at boot, so a trace-driven bring-up
misses it and the port simply never links.

**The CRM is optional.** Its memory fills can be done as direct MMIO writes.
Independently confirmed: the datasheet's step 12 offers "software writes memory
manually" as an alternative to driving the CRM.

**The packet DMA block is at BAR0 byte offset `0x5000`**, not a word address
like everything else in the register space.

**The L2F dmask table is at `0x180000 + 4*idx`.**

**Reading `ESCHED` at `0x2000` off-buses a cold chip.** A probe that reads it
during bring-up kills the thing it is probing.

**Corrections they paid for, which should not be re-derived:** SerDes core
`reg0x22` is telemetry and not an enable; SBus read-back is not a valid verifier
because the vendor's own writes do not read back either; zero `STATS_AR` port
maps are normal on a working chip; and an `0xffffffff` written to an 18-bit
field reading back as `0x0003ffff` is correct truncation, not corruption.

## Not taken, and why

**The register replay.** Their working sequence ends with roughly 394,000
captured EOS writes played back at the chip. It works, and NOSaic will not do
it. A replay cannot take a port map, cannot be told to bring up ten ports
instead of fifty-two, cannot be explained to the next person, and carries
whatever licence the capture carries. It stays an **oracle** — the best
available ground truth for what a configured FM6000 looks like, to diff our own
chip state against — and never something emitted.

**Warm-inherit.** Booting without a power cycle so the chip keeps EOS's
configuration proves a byte-mover and produces a switch that needs the vendor OS
to start. Their own notes call it a diagnostic rather than a product path.

**Arista's parser microcode.** Marked *Confidential and Proprietary*, so usable
for research on one's own box and not shippable. NOSaic generates its own from
the documented encoding instead — see
[hardware.md](hardware.md#the-parser-is-microcoded-and-we-write-the-microcode).

## Where NOSaic should not repeat the route taken

Their hardest fight was the cold bank-memory wall, across roughly twenty-five
numbered phases: scan-chain disassembly, a 6287-iteration configuration program
found in the vendor SDK, BIST marches, CRM descriptors, fusebox repair theories.
It ended in a sequence that works but is described in their own notes as
non-deterministic and incomplete.

**The datasheet gives an ordered twelve-step boot for this, and it does not
appear to have been followed in that order.** Steps 8 to 10 are `BOOT_CTRL`
commands — initialize FFU slice numbers, **apply bank memory repairs**,
initialize scheduler freelists — each polled on `BOOT_STATUS:CommandDone`, and
they come after the scan-chain write and the PLL and before anything touches a
memory.

There is a genuine conflict to resolve rather than assume away: step 5 is a
single write of `0xFFFFFFFF` to `SCAN_CHAIN_DATA_IN`, where their phase-78
conclusion was that a per-block scan *program* is what actually makes memory
writable and that the single write is not enough. Both can be true if the vendor
applies fusebox trim on top of the documented minimum. **Run the documented
order first and find out** — it is the experiment that decides whether this
board's M2 is a day or a month, and it is cheap.

## One to watch: the frame tag width

Their live snoop of a working transmit put the F64 tag at **8 bytes** at L2
offset 12. The datasheet's Table 7-8 describes **7**. A tag wrong by one byte
produces frames the chip accepts and misparses, so this is worth settling
deliberately rather than discovering.
