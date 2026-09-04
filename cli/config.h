/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NOSAIC_CLI_CONFIG_H
#define NOSAIC_CLI_CONFIG_H

#include <stddef.h>

/* What the image shipped, and what this switch is. See config.c. */
#define NOSAIC_CONFIG_IMAGE_DIR "/etc/nosaic"
#define NOSAIC_CONFIG_SITE_DIR  "/mnt/data/config"

/* Where `config set` writes. One file rather than per-subject files, because a
 * switch's own settings are few and having them in one place makes "what is
 * different about this box" a single cat. */
#define NOSAIC_CONFIG_SITE_FILE "local.conf"

void        nosaic_config_load(void);
int         nosaic_config_count(void);
const char *nosaic_config_name(int i);
const char *nosaic_config_value(int i);
const char *nosaic_config_file(int i);
int         nosaic_config_is_site(int i);
const char *nosaic_config_get(const char *name);

/* Write or, with a NULL value, remove a setting. Returns 0, or -1 with a
 * message in err. */
int nosaic_config_set(const char *name, const char *value, char *err, size_t errlen);

#endif
