# Building NOSaic for Dell S6000-ON

## Build the image

```sh
make image BOARD=dell-s6000-on
```

The output installer is `out/images/dell-s6000-on/NOSaic-<version>-dell-s6000-on.bin`.

## Extracting `portmap.conf`

If you have SONiC (or ONL) running on the switch, extract the Broadcom SDK port
map and SerDes polarity from `td2-s6000-32x40G.config.bcm` using the read-only
helper in `tools/`:

```sh
./platform/dell-s6000-on/tools/mkportmap.sh --stdin < td2-s6000-32x40G.config.bcm > platform/dell-s6000-on/config/portmap.conf
```

To map all 32 QSFP+ cages as 4×10G breakout ports instead of 40G:

```sh
QSFP_BREAKOUT=1 ./platform/dell-s6000-on/tools/mkportmap.sh --stdin < td2-s6000-32x40G.config.bcm > platform/dell-s6000-on/config/portmap.conf
```
