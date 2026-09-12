/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Chip properties, read from key=value files.
 *
 * Shared between ASIC datapaths because a properties file reader is not
 * ASIC-specific -- the third thing to end up here, after MMIO ordering, and
 * pointing the same way: what these two daemons have in common is the
 * plumbing, and what differs is the silicon.
 */
#ifndef NOSAIC_PROPS_H
#define NOSAIC_PROPS_H

/*
 * Where a switch's own configuration lives, as opposed to the image's.
 *
 * On the shared data partition, so it survives both an upgrade and a rollback:
 * config that lived in a slot would be lost by the first and reverted by the
 * second, which is the whole reason the partition is shared.
 */
#define NOSAIC_CONFIG_DIR "/mnt/data/config"

/* Load key=value properties. Returns how many were read, or -1. */
int nosaic_props_load(const char *path);

/* Look one up, or NULL if it is not set -- which the SDK reads as "use the
 * default for this chip". */
const char *nosaic_props_get(const char *name);

/*
 * Look up a property the way the SDK's own configuration layer does: the
 * unit-suffixed name first, then the bare one.
 *
 * ⚠ A PROPERTY FILE FROM A REAL SWITCH IS SUFFIXED, AND AN EXACT MATCH MISSES
 * EVERY LINE OF IT.
 *
 * Broadcom's config.bcm writes "portmap_1.0", where the ".0" is the unit. A
 * map generated from a switch running the vendor OS therefore arrives suffixed
 * -- and on the 7050TX-64 all 278 port-map properties and all 104 polarity
 * properties loaded, were counted, were reported on the console, and were then
 * invisible to every lookup in the daemon. "no port map" printed immediately
 * under "278 properties from /etc/nosaic/portmap.conf".
 *
 * That is the failure props_report_unused exists to catch, arriving from the
 * other side: not a property the SDK never asked for, but a property the SDK
 * asked for under a name nothing here would match.
 */
const char *nosaic_props_get_unit(const char *name, int unit);

/* Load every *.conf in a directory, in name order. Returns how many
 * properties were read, or -1 if the directory is not there. */
int nosaic_props_load_dir(const char *dir);

int nosaic_props_count(void);

/* Iterate the loaded properties by index. */
const char *nosaic_props_name(int i);
const char *nosaic_props_value(int i);

/* Print any properties the SDK never asked for. Call after initialisation. */
void nosaic_props_report_unused(void);

#endif
