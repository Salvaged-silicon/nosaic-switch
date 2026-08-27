# Installing NOSaic on <Board>

> Delete this line when the page is filled in.

Written for somebody holding the switch who has never seen it before. Assume a
console cable and nothing else. Prefer exact commands and exact expected output
over description — a reader who cannot tell whether a step worked is stuck.

## Before you start

State plainly what this will destroy, and what it will not. Installing a NOS
over a vendor OS is not reversible without the vendor image, so say where to get
that image back and confirm the reader has it.

## Console

Device, baud, data bits, flow control. Which physical connector, and whether it
needs a specific adapter.

## Getting the image onto the box

The mechanism this board's bootloader offers — USB, TFTP, HTTP, ONIE's
installer, an existing shell. Include the exact commands.

## Installing

Step by step. After each step, what you should see.

## First boot

What normal looks like, and roughly how long it takes. Log in as `admin`; there
is no password until you set one with `passwd`.

## Going back to the vendor OS

Required, not optional. Somebody will need it, and needing it means something
has already gone wrong.

## When it does not work

The failures seen during bring-up on *this* board, and what each one means.
Symptom first — the reader has a symptom, not a diagnosis.
