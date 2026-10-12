/* Optional settings from /data/fgg-xsense/config.txt.
 *
 * One `key=value` per line; `#` starts a comment. A missing file or key just
 * means the default, so the payload never needs the file to run.
 */
#ifndef FGG_CONFIG_H
#define FGG_CONFIG_H

#define CONFIG_PATH "/data/fgg-xsense/config.txt"

void config_load(const char *path);
int  config_int(const char *key, int fallback);

#endif
