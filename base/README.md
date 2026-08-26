# base/ — base system profiles

The staged package sets that make a bootable userspace, in three tiers:

| Profile | libc | init | For |
|---------|------|------|-----|
| `full` | glibc | systemd | Boards with room to spare |
| `slim` | glibc | systemd | The common case |
| `minimal` | glibc | busybox + s6 | Small flash and RAM |

All three share the same recipes and differ only in package set and init. This is
why recipes declare services abstractly instead of shipping unit files — see
CONTRIBUTING.md.

Populated at M3.
