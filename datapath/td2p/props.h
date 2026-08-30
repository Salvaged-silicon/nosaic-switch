/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_TD2P_PROPS_H
#define NOSAIC_TD2P_PROPS_H

/* Load key=value properties. Returns how many were read, or -1. */
int nosaic_props_load(const char *path);

/* Look one up, or NULL if it is not set -- which the SDK reads as "use the
 * default for this chip". */
const char *nosaic_props_get(const char *name);

int nosaic_props_count(void);

/* Print any properties the SDK never asked for. Call after initialisation. */
void nosaic_props_report_unused(void);

#endif
