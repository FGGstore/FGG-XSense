#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEYS 32
#define KEY_LEN  32
#define VAL_LEN  64

static char g_keys[MAX_KEYS][KEY_LEN];
static char g_vals[MAX_KEYS][VAL_LEN];
static int  g_count;

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                     e[-1] == '\n'))
        *--e = '\0';
    return s;
}

void config_load(const char *path)
{
    char line[128];
    FILE *f = fopen(path, "r");

    g_count = 0;
    if (!f) return;

    while (g_count < MAX_KEYS && fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#'), *eq, *key, *val;

        if (hash) *hash = '\0';
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';

        key = trim(line);
        val = trim(eq + 1);
        if (*key == '\0') continue;

        snprintf(g_keys[g_count], KEY_LEN, "%s", key);
        snprintf(g_vals[g_count], VAL_LEN, "%s", val);
        g_count++;
    }
    fclose(f);
}

int config_int(const char *key, int fallback)
{
    int i;

    for (i = 0; i < g_count; i++)
        if (strcmp(g_keys[i], key) == 0) return atoi(g_vals[i]);
    return fallback;
}
