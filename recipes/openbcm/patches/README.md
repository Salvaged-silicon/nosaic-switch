# Patches to the OpenBCM SDK

Three kinds of change end up here, and only three:

1. **Build-environment repairs** — the SDK is from an era of older make,
   older gcc and a glibc that still had `libnsl`. These are not fixes to
   Broadcom's logic and must never become them.
2. **Nothing else yet.**

Patches are generated against **sdk-6.5.24** and will not apply to another
version. That is deliberate: the first attempt here was written from a script
targeting 6.5.16, where the same repairs are spelled differently and an
`ALLOWED_MAKE_VERSIONS` list exists that 6.5.24 has dropped. It failed to apply,
which is the correct outcome — a patch that applied loosely across versions
would be a patch that silently edited the wrong line.

What must *not* go here: chip logic. The reason for taking the SDK route at all
is that Broadcom's own initialisation sequences run unmodified — the Trident2
MMU and LLS setup are precisely the parts that hand-reproduction failed to
match six times. A patch that changes chip behaviour forfeits that and should
be a hook in our own code instead, as the reverse-engineering work did with two
weak symbols rather than by editing the SDK.
