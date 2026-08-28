/* game_config.c — minimal TOML-subset parser for game declarations.
 *
 * Supports exactly what game.toml needs and nothing more:
 *   # comments
 *   [section]
 *   [[array_of_tables]]
 *   key = "string"
 *   key = 123          key = 0x06004000
 *   key = true|false
 *   key = [0x100, 0x200, ...]      (integer arrays, may span lines)
 */
#include "game_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char *trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s)) s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static void strip_comment(char *s)
{
    int in_str = 0;
    for (; *s; s++) {
        if (*s == '"') in_str = !in_str;
        else if (*s == '#' && !in_str) { *s = 0; return; }
    }
}

static int parse_uint(const char *v, uint32_t *out)
{
    char *end;
    unsigned long x;
    if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) x = strtoul(v + 2, &end, 16);
    else                                             x = strtoul(v, &end, 10);
    if (end == v) return -1;
    *out = (uint32_t)x;
    return 0;
}

static void unquote(const char *v, char *out, size_t n)
{
    size_t len;
    if (*v == '"') v++;
    snprintf(out, n, "%s", v);
    len = strlen(out);
    if (len && out[len-1] == '"') out[len-1] = 0;
}

static void resolve_rel(const char *base_file, const char *rel, char *out, size_t n)
{
    const char *a = strrchr(base_file, '/');
    const char *b = strrchr(base_file, '\\');
    const char *cut = a > b ? a : b;
    int absolute = rel[0] == '/' || rel[0] == '\\' ||
                   (isalpha((unsigned char)rel[0]) && rel[1] == ':');
    if (absolute || !cut) { snprintf(out, n, "%s", rel); return; }
    {
        size_t d = (size_t)(cut - base_file) + 1;
        if (d >= n) d = n - 1;
        memcpy(out, base_file, d);
        snprintf(out + d, n - d, "%s", rel);
    }
}

int gc_load(game_config *g, const char *toml_path)
{
    FILE *f;
    char line[1024];
    enum { S_NONE, S_GAME, S_MODULE } sect = S_NONE;
    gc_module *mod = NULL;
    int in_array = 0;

    memset(g, 0, sizeof(*g));
    f = fopen(toml_path, "r");
    if (!f) { snprintf(g->err, sizeof(g->err), "cannot open %s", toml_path); return -1; }

    while (fgets(line, sizeof(line), f)) {
        char *s;
        char *eq, *key, *val;

        strip_comment(line);
        s = trim(line);
        if (!*s) continue;

        /* Continuation of a multi-line integer array. */
        if (in_array) {
            char *p = s;
            while (*p) {
                uint32_t v;
                char tok[64]; int i = 0;
                while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
                if (*p == ']') { in_array = 0; break; }
                while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != ']'
                       && i < (int)sizeof(tok) - 1)
                    tok[i++] = *p++;
                tok[i] = 0;
                if (i && parse_uint(tok, &v) == 0 && mod &&
                    mod->nentries < GC_MAX_ENTRIES)
                    mod->entries[mod->nentries++] = v;
            }
            continue;
        }

        if (s[0] == '[') {
            if (strncmp(s, "[[module]]", 10) == 0) {
                if (g->nmodules >= GC_MAX_MODULES) {
                    snprintf(g->err, sizeof(g->err), "too many modules");
                    fclose(f); return -1;
                }
                mod  = &g->modules[g->nmodules++];
                memset(mod, 0, sizeof(*mod));
                sect = S_MODULE;
            } else if (strncmp(s, "[game]", 6) == 0) {
                sect = S_GAME;
                mod  = NULL;
            } else {
                sect = S_NONE;
                mod  = NULL;
            }
            continue;
        }

        eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        key = trim(s);
        val = trim(eq + 1);

        if (sect == S_GAME) {
            if      (!strcmp(key, "name"))       unquote(val, g->name, sizeof(g->name));
            else if (!strcmp(key, "prefix"))     unquote(val, g->prefix, sizeof(g->prefix));
            else if (!strcmp(key, "product_no")) unquote(val, g->product_no, sizeof(g->product_no));
            else if (!strcmp(key, "disc")) {
                char raw[512];
                unquote(val, raw, sizeof(raw));
                resolve_rel(toml_path, raw, g->disc, sizeof(g->disc));
            } else if (!strcmp(key, "bios")) {
                char raw[512];
                unquote(val, raw, sizeof(raw));
                resolve_rel(toml_path, raw, g->bios, sizeof(g->bios));
            }
        } else if (sect == S_MODULE && mod) {
            if      (!strcmp(key, "name")) unquote(val, mod->name, sizeof(mod->name));
            else if (!strcmp(key, "file")) unquote(val, mod->file, sizeof(mod->file));
            else if (!strcmp(key, "load_addr")) parse_uint(val, &mod->load_addr);
            else if (!strcmp(key, "entry"))     parse_uint(val, &mod->entry);
            else if (!strcmp(key, "first_read"))
                mod->is_first_read = (!strcmp(val, "true") || !strcmp(val, "1"));
            else if (!strcmp(key, "cpu")) {
                char t[32]; unquote(val, t, sizeof(t));
                mod->cpu = (!strcmp(t, "m68k") || !strcmp(t, "68000")) ? GC_CPU_M68K : GC_CPU_SH2;
            } else if (!strcmp(key, "compression")) {
                char t[32]; unquote(val, t, sizeof(t));
                mod->compression = !strcmp(t, "prs") ? GC_COMP_PRS : GC_COMP_NONE;
            } else if (!strcmp(key, "entries")) {
                char *p = strchr(val, '[');
                in_array = 1;
                if (p) {
                    p++;
                    while (*p) {
                        uint32_t v; char tok[64]; int i = 0;
                        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
                        if (*p == ']') { in_array = 0; break; }
                        while (*p && !isspace((unsigned char)*p) && *p != ',' &&
                               *p != ']' && i < (int)sizeof(tok) - 1)
                            tok[i++] = *p++;
                        tok[i] = 0;
                        if (i && parse_uint(tok, &v) && mod->nentries < GC_MAX_ENTRIES)
                            ;   /* parse failed, skip */
                        else if (i && mod->nentries < GC_MAX_ENTRIES) {
                            parse_uint(tok, &v);
                            mod->entries[mod->nentries++] = v;
                        }
                    }
                }
            }
        }
    }
    fclose(f);

    if (!g->name[0])   snprintf(g->name, sizeof(g->name), "(unnamed)");
    if (!g->prefix[0]) snprintf(g->prefix, sizeof(g->prefix), "game");
    if (!g->disc[0]) {
        snprintf(g->err, sizeof(g->err), "game.toml has no [game] disc = \"...\"");
        return -1;
    }
    return 0;
}

const char *gc_compression_name(gc_compression c)
{
    return c == GC_COMP_PRS ? "prs" : "none";
}

const char *gc_cpu_name(gc_cpu c)
{
    return c == GC_CPU_M68K ? "m68k" : "sh2";
}
