/* Custom scenarios (.ma2map) -- the game side of the MechAssault 2 Scenario Editor.
 *
 * Contract: docs/editor.md (file formats). The editor saves one .ma2map per scenario
 * (an uncompressed MGF v4 archive holding scenario.ini, world.wdf and the scenario's own files)
 * into the scenario folder. At boot this module turns every valid file into data the original
 * loaders already understand:
 *
 *   T:\scenarios\index_<n>.mgf   mounted right after Common.mgf (recomp_0000.c, 3 copies), holds
 *       \Multiplay\MissionSet63.ini   the custom MP maps -- the map-set loader sub_001630FA tries
 *                                     MissionSet00..63 at boot (the hook downloadable content used)
 *       \Multiplay\Scn<ID>.ini        map ini in the retail format (WorldFile, ResourceFile, MapName...)
 *       \Multiplay\Scn<ID>.wdf        world.wdf with its custom text/cue ids made global (below)
 *       the scenario's own files      Terrain\Scn<ID>_*.tif, Pathfinding\Scn<ID>_*.map, ...
 *   T:\scenarios\Scn<ID>_assets_<hash>.mgf  only when the scenario uses retail files outside its
 *       Base archive ([Assets]): just those files. ResourceFile is always Base itself, so the level
 *       archive is cached and mounted natively; the assets archive is mounted beside it while the
 *       level is loaded (ma2_scn_level_load / ma2_scn_level_unload, recomp_0000.c).
 *
 * T:\ is <save dir>\TitleData on the host (kernel_path.c rule), so these are plain host files.
 * The level loader turns ResourceFile into "<root><name>.mgf" (sub_00165244 / sub_001652D8,
 * recomp_0037.c); ma2_scn_resource_path() sends names starting "Scn" to T:\scenarios\.
 *
 * Text: the editor writes scenario-local text ids 0x00F00000+n ([Strings] n) and cue ids
 * 0x5C000000+n ([Audio] n). Several scenarios share those, so the world file copied into the
 * index gets global ones: text 0x00F10000 + i*0x1000 + n, cues 0x5C100000 + i*0x1000 + n
 * (i = scenario slot). Map names/descriptions are 0x00FF0000 + 2i (+1). ma2_pc_localized
 * (pc_prompts.cpp) asks ma2_scn_text() for the whole 0x00F00000..0x00FFFFFF range.
 *
 * Diagnostics: [SCENARIO] lines on stderr. RECOMP_SCENARIOS=0 disables everything;
 * RECOMP_SCENARIO_DIR overrides the folder (tests).
 */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <psapi.h>
#include <knownfolders.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <time.h>
/* Log privacy (kernel_thunks.c): a path for a log line, with the player's profile folders written as
 * %LOCALAPPDATA% / %APPDATA% / %USERPROFILE% (players post logs publicly). */
const char *xbox_log_path(const char *path, char *out, size_t size);
const char *xbox_log_pathw(const wchar_t *path, char *out, size_t size);
void xbox_log_redact(char *text, size_t size);
#include "recomp/gen/recomp_types.h"
#include "recomp/gen/recomp_funcs.h"
#include "ma2_scenario.h"

#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
#define esp g_esp

extern BOOL xbox_translate_path(const char *xbox_path, WCHAR *host_path_buf, DWORD buf_size);
uint32_t ma2_pc_wide(const wchar_t *text);   /* pc_prompts.cpp: wide string -> guest pointer */

#define SCN_MAX          200
#define SCN_TEXT_LOCAL   0x00F00000u
#define SCN_TEXT_GLOBAL  0x00F10000u
#define SCN_NAME_BASE    0x00FF0000u
#define SCN_CUE_LOCAL    0x5C000000u
#define SCN_CUE_GLOBAL   0x5C100000u
#define SCN_LOCAL_MAX    0x0FFFu   /* cue n */
#define SCN_TEXT_LOCAL_MAX 0xFFFFu /* [Strings] n */

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */
static void logf_(const char *fmt, ...)
{
    char line[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    xbox_log_redact(line, sizeof line);          /* scenario and folder paths: no user name in the log */
    fputs("[SCENARIO] ", stderr); fputs(line, stderr); fputc('\n', stderr); fflush(stderr);
}

static uint32_t crc_table[256];
static void crc_init(void)
{
    if (crc_table[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
/* zlib crc32() of data */
static uint32_t crc32_of(const void *data, size_t n)
{
    crc_init();
    const uint8_t *p = (const uint8_t *)data; uint32_t c = 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
/* The engine's path/class key: crc32 without the final complement, of the lower-case path. */
static uint32_t path_key(const char *path)
{
    crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (const char *p = path; *p; p++) {
        char ch = (char)tolower((unsigned char)(*p == '/' ? '\\' : *p));
        c = crc_table[(c ^ (uint8_t)ch) & 0xFF] ^ (c >> 8);
    }
    return c;
}

static uint8_t *read_file_w(const wchar_t *path, uint32_t *size)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER len; uint8_t *data = NULL; DWORD got = 0;
    if (GetFileSizeEx(h, &len) && len.QuadPart > 0 && len.QuadPart < (512ll << 20)) {
        data = (uint8_t *)malloc((size_t)len.QuadPart + 1);
        if (data && ReadFile(h, data, (DWORD)len.QuadPart, &got, NULL) && got == (DWORD)len.QuadPart) {
            data[got] = 0; *size = got;
        } else { free(data); data = NULL; }
    }
    CloseHandle(h);
    return data;
}

static int write_file_w(const wchar_t *path, const void *data, size_t size)
{
    wchar_t tmp[MAX_PATH];
    swprintf(tmp, MAX_PATH, L"%ls.tmp", path);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0; BOOL ok = WriteFile(h, data, (DWORD)size, &put, NULL) && put == (DWORD)size;
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp); return 0; }
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp); return 0; }
    return 1;
}

static int istarts(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
    return 1;
}

/* Growable byte buffer */
typedef struct Buf { uint8_t *p; size_t n, cap; } Buf;
static void buf_reserve(Buf *b, size_t extra)
{
    if (b->n + extra <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + extra) cap *= 2;
    uint8_t *p = (uint8_t *)realloc(b->p, cap);
    if (!p) { fprintf(stderr, "[SCENARIO] out of memory\n"); abort(); }
    b->p = p; b->cap = cap;
}
static void buf_put(Buf *b, const void *data, size_t n) { buf_reserve(b, n); memcpy(b->p + b->n, data, n); b->n += n; }
static void buf_u32(Buf *b, uint32_t v) { buf_put(b, &v, 4); }
static void buf_str(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }
static void buf_pad(Buf *b, size_t align) { static const uint8_t z[64]; while (b->n % align) buf_put(b, z, 1); }

/* ------------------------------------------------------------------ */
/* Inflate (RFC 1950/1951), copied from bots/ma2_bots_core.c           */
/* ------------------------------------------------------------------ */
typedef struct Inf { const uint8_t *in; size_t inlen, inpos; uint32_t bb; int bc; uint8_t *out; size_t outlen, outpos; int err; } Inf;
typedef struct Huff { short count[16]; short symbol[320]; } Huff;

static int inf_bits(Inf *s, int n)
{
    uint32_t v = s->bb;
    while (s->bc < n) {
        if (s->inpos >= s->inlen) { s->err = 1; return 0; }
        v |= (uint32_t)s->in[s->inpos++] << s->bc;
        s->bc += 8;
    }
    s->bb = n < 32 ? v >> n : 0;
    s->bc -= n;
    return (int)(v & ((1u << n) - 1u));
}
static int huff_build(Huff *h, const short *len, int n)
{
    short offs[16];
    memset(h->count, 0, sizeof(h->count));
    for (int s = 0; s < n; s++) h->count[len[s]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int i = 1; i < 16; i++) { left <<= 1; left -= h->count[i]; if (left < 0) return -1; }
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (int s = 0; s < n; s++) if (len[s]) h->symbol[offs[len[s]]++] = (short)s;
    return left;
}
static int huff_decode(Inf *s, const Huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= inf_bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
static const short k_lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short k_lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short k_dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short k_dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static int inf_codes(Inf *s, const Huff *lc, const Huff *dc)
{
    for (;;) {
        int sym = huff_decode(s, lc);
        if (s->err || sym < 0) return -1;
        if (sym < 256) {
            if (s->outpos >= s->outlen) return -1;
            s->out[s->outpos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = k_lbase[sym] + inf_bits(s, k_lext[sym]);
            int ds = huff_decode(s, dc);
            if (s->err || ds < 0 || ds >= 30) return -1;
            size_t dist = (size_t)(k_dbase[ds] + inf_bits(s, k_dext[ds]));
            if (s->err || dist > s->outpos || s->outpos + (size_t)len > s->outlen) return -1;
            while (len--) { s->out[s->outpos] = s->out[s->outpos - dist]; s->outpos++; }
        }
    }
}
static int inf_block_stored(Inf *s)
{
    s->bb = 0; s->bc = 0;
    if (s->inpos + 4 > s->inlen) return -1;
    unsigned len = s->in[s->inpos] | (s->in[s->inpos + 1] << 8);
    unsigned nlen = s->in[s->inpos + 2] | (s->in[s->inpos + 3] << 8);
    s->inpos += 4;
    if (len != (~nlen & 0xFFFFu) || s->inpos + len > s->inlen || s->outpos + len > s->outlen) return -1;
    memcpy(s->out + s->outpos, s->in + s->inpos, len);
    s->inpos += len; s->outpos += len;
    return 0;
}
static int inf_block_fixed(Inf *s)
{
    static Huff lc, dc; static int built;
    if (!built) {
        short l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        huff_build(&lc, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        huff_build(&dc, l, 30);
        built = 1;
    }
    return inf_codes(s, &lc, &dc);
}
static int inf_block_dynamic(Inf *s)
{
    static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    short lengths[320];
    Huff lc, dc;
    int nlen = inf_bits(s, 5) + 257, ndist = inf_bits(s, 5) + 1, ncode = inf_bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) return -1;
    int i = 0;
    for (; i < ncode; i++) lengths[order[i]] = (short)inf_bits(s, 3);
    for (; i < 19; i++) lengths[order[i]] = 0;
    if (s->err || huff_build(&lc, lengths, 19) != 0) return -1;
    i = 0;
    while (i < nlen + ndist) {
        int sym = huff_decode(s, &lc);
        if (s->err || sym < 0) return -1;
        if (sym < 16) { lengths[i++] = (short)sym; continue; }
        int len = 0, rep;
        if (sym == 16) { if (i == 0) return -1; len = lengths[i - 1]; rep = 3 + inf_bits(s, 2); }
        else if (sym == 17) rep = 3 + inf_bits(s, 3);
        else rep = 11 + inf_bits(s, 7);
        if (s->err || i + rep > nlen + ndist) return -1;
        while (rep--) lengths[i++] = (short)len;
    }
    if (lengths[256] == 0) return -1;
    int err = huff_build(&lc, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lc.count[0] != 1)) return -1;
    err = huff_build(&dc, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - dc.count[0] != 1)) return -1;
    return inf_codes(s, &lc, &dc);
}
static long zlib_inflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen)
{
    if (inlen < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) return -1;
    Inf s; memset(&s, 0, sizeof(s));
    s.in = in; s.inlen = inlen; s.inpos = 2; s.out = out; s.outlen = outlen;
    int last;
    do {
        last = inf_bits(&s, 1);
        int type = inf_bits(&s, 2);
        if (s.err) return -1;
        int rc = type == 0 ? inf_block_stored(&s) : type == 1 ? inf_block_fixed(&s) : type == 2 ? inf_block_dynamic(&s) : -1;
        if (rc != 0) return -1;
    } while (!last);
    return (long)s.outpos;
}

/* ------------------------------------------------------------------ */
/* MGF v4 archives                                                     */
/* ------------------------------------------------------------------ */
/* Header: 'mgf ', 0x5A5A0104, 0x10, file count/bytes/offset, dir count/bytes/offset,
 *   string bytes/offset, then 5 more words (64 bytes in all).
 * File record (32): dir row, crc32(data)^~0? -- stored as zlib crc32 (= ~engine crc),
 *   path key = crc32 of lower-case "\path\file" (zlib form), size, size, unix time, offset, 0x12F700.
 * Dir row (24): key of "\dir\path" for directories (0 for files), parent, first child,
 *   next sibling, name offset, file index (-1 for directories).
 * Strings: "MGI\0", then NUL-terminated names each padded to 4 bytes.
 * Every value above checked against retail Common/Templates/BoggedDown (mgf_dirhash.py). */
typedef struct MgfEntry { char path[192]; uint32_t offset, size; } MgfEntry;
typedef struct Mgf {
    uint8_t *raw; uint32_t raw_size;     /* whole archive, decompressed */
    MgfEntry *e; int n;
} Mgf;

static void mgf_free(Mgf *m) { free(m->raw); free(m->e); memset(m, 0, sizeof(*m)); }

/* Loads (and if needed decompresses) a whole archive held in `file` (taken over). */
static int mgf_load(Mgf *m, uint8_t *file, uint32_t size)
{
    memset(m, 0, sizeof(*m));
    if (size >= 44 && !memcmp(file, "mgf ", 4)) {
        m->raw = file; m->raw_size = size;
    } else if (size >= 32 && *(uint32_t *)(file + 4) == 0xC5735A0Fu) {
        uint32_t count = *(uint32_t *)(file + 8), total = *(uint32_t *)(file + 12), block = *(uint32_t *)(file + 20);
        if (!count || count > 200000 || !block || block > (16u << 20) || 32ull + count * 12ull > size || total > (600u << 20)) { free(file); return 0; }
        uint8_t *out = (uint8_t *)malloc((size_t)total + block);
        if (!out) { free(file); return 0; }
        uint32_t pos = 0;
        for (uint32_t i = 0; i < count && pos < total; i++) {
            const uint32_t *c = (const uint32_t *)(file + 32 + i * 12);
            if ((uint64_t)c[0] + c[1] > size) { free(out); free(file); return 0; }
            long got = zlib_inflate(file + c[0], c[1], out + pos, block);
            if (got <= 0) { free(out); free(file); return 0; }
            pos += (uint32_t)got;
        }
        free(file);
        m->raw = out; m->raw_size = pos;
    } else { free(file); return 0; }
    const uint32_t *h = (const uint32_t *)m->raw;
    uint32_t fc = h[3], fo = h[5], dc = h[6], dof = h[8], sl = h[9], so = h[10];
    if (h[0] != 0x2066676Du || (uint64_t)fo + fc * 32ull > m->raw_size || (uint64_t)dof + dc * 24ull > m->raw_size ||
        (uint64_t)so + sl > m->raw_size || fc > 200000 || dc > 400000) { mgf_free(m); return 0; }
    const uint32_t *rec = (const uint32_t *)(m->raw + fo);
    const int32_t *row = (const int32_t *)(m->raw + dof);
    const char *str = (const char *)(m->raw + so);
    m->e = (MgfEntry *)calloc(fc ? fc : 1, sizeof(MgfEntry));
    if (!m->e) { mgf_free(m); return 0; }
    for (uint32_t f = 0; f < fc; f++) {
        const uint32_t *r = rec + f * 8;
        if (r[0] >= dc || (uint64_t)r[6] + r[3] > m->raw_size) continue;
        /* rebuild the path from the dir rows (root name "\" is skipped) */
        const char *parts[32]; int np = 0; int32_t i = (int32_t)r[0];
        while (i > 0 && np < 32) {
            const int32_t *rw = row + i * 6;
            if ((uint32_t)rw[4] >= sl) break;
            parts[np++] = str + rw[4];
            i = rw[1];
            if (i >= (int32_t)dc) break;
        }
        MgfEntry *e = &m->e[m->n];
        e->path[0] = 0;
        for (int k = np - 1; k >= 0; k--) {
            if (strlen(e->path) + strlen(parts[k]) + 2 >= sizeof(e->path)) break;
            strcat(e->path, parts[k]);
            if (k) strcat(e->path, "/");
        }
        e->offset = r[6]; e->size = r[3];
        m->n++;
    }
    return 1;
}

static const MgfEntry *mgf_find(const Mgf *m, const char *path)
{
    for (int i = 0; i < m->n; i++) {
        const char *a = m->e[i].path, *b = path;
        while (*b == '/' || *b == '\\') b++;
        for (;; a++, b++) {
            char ca = (char)tolower((unsigned char)(*a == '\\' ? '/' : *a));
            char cb = (char)tolower((unsigned char)(*b == '\\' ? '/' : *b));
            if (ca != cb) break;
            if (!ca) return &m->e[i];
        }
    }
    return NULL;
}

/* Archive writer: collects (path, data) and writes one plain archive. */
typedef struct WEntry { char path[192]; const uint8_t *data; uint32_t size; } WEntry;
typedef struct Writer { WEntry *e; int n, cap; } Writer;

static void w_add(Writer *w, const char *path, const uint8_t *data, uint32_t size)
{
    while (*path == '/' || *path == '\\') path++;
    for (int i = 0; i < w->n; i++)          /* later adds replace earlier ones */
        if (!_stricmp(w->e[i].path, path)) { w->e[i].data = data; w->e[i].size = size; return; }
    if (w->n == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 256;
        w->e = (WEntry *)realloc(w->e, (size_t)w->cap * sizeof(WEntry));
        if (!w->e) abort();
    }
    WEntry *e = &w->e[w->n++];
    snprintf(e->path, sizeof(e->path), "%s", path);
    for (char *p = e->path; *p; p++) if (*p == '\\') *p = '/';
    e->data = data; e->size = size;
}

typedef struct Node { char name[96]; int parent, first, last, next, file, is_dir; uint32_t name_off; } Node;

static int node_child(Node **nodes, int *nn, int *cap, int parent, const char *name, int is_dir)
{
    for (int c = (*nodes)[parent].first; c >= 0; c = (*nodes)[c].next)
        if ((*nodes)[c].is_dir == is_dir && !_stricmp((*nodes)[c].name, name)) return c;
    if (*nn == *cap) { *cap *= 2; *nodes = (Node *)realloc(*nodes, (size_t)*cap * sizeof(Node)); if (!*nodes) abort(); }
    Node *n = &(*nodes)[*nn];
    memset(n, 0, sizeof(*n));
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->parent = parent; n->first = n->last = n->next = -1; n->file = -1; n->is_dir = is_dir;
    if ((*nodes)[parent].last >= 0) (*nodes)[(*nodes)[parent].last].next = *nn;
    else (*nodes)[parent].first = *nn;
    (*nodes)[parent].last = *nn;
    return (*nn)++;
}

static void node_path(const Node *nodes, int i, char *out, size_t cap)
{
    const char *parts[32]; int np = 0;
    for (int k = i; k > 0 && np < 32; k = nodes[k].parent) parts[np++] = nodes[k].name;
    size_t len = 0; out[0] = 0;
    for (int k = np - 1; k >= 0; k--) len += (size_t)snprintf(out + len, len < cap ? cap - len : 0, "\\%s", parts[k]);
    if (!np) snprintf(out, cap, "\\");
}

static int w_write(Writer *w, const wchar_t *host_path)
{
    int cap = 64, nn = 1;
    Node *nodes = (Node *)calloc((size_t)cap, sizeof(Node));
    if (!nodes) return 0;
    snprintf(nodes[0].name, sizeof(nodes[0].name), "\\");
    nodes[0].parent = -1; nodes[0].first = nodes[0].last = nodes[0].next = -1; nodes[0].file = -1; nodes[0].is_dir = 1;
    for (int f = 0; f < w->n; f++) {
        char tmp[192]; snprintf(tmp, sizeof(tmp), "%s", w->e[f].path);
        int parent = 0; char *seg = tmp;
        for (char *p = tmp; ; p++) {
            if (*p == '/' || !*p) {
                int last = !*p; *p = 0;
                if (*seg) {
                    int c = node_child(&nodes, &nn, &cap, parent, seg, !last);
                    if (last) nodes[c].file = f;
                    parent = c;
                }
                if (last) break;
                seg = p + 1;
            }
        }
    }
    /* Rows in DFS order so a parent always precedes its children. */
    int *order = (int *)malloc((size_t)nn * sizeof(int)), *row_of = (int *)malloc((size_t)nn * sizeof(int));
    int *stack = (int *)malloc((size_t)nn * sizeof(int));
    int no = 0, sp = 0;
    stack[sp++] = 0;
    while (sp) {
        int i = stack[--sp];
        row_of[i] = no; order[no++] = i;
        int kids[4096], nk = 0;
        for (int c = nodes[i].first; c >= 0 && nk < 4096; c = nodes[c].next) kids[nk++] = c;
        for (int k = nk - 1; k >= 0; k--) stack[sp++] = kids[k];
    }
    Buf strings = {0};
    buf_str(&strings, "MGI"); buf_put(&strings, "", 1);
    for (int r = 0; r < no; r++) {
        Node *n = &nodes[order[r]];
        n->name_off = (uint32_t)strings.n;
        buf_str(&strings, n->name); buf_put(&strings, "", 1); buf_pad(&strings, 4);
    }
    uint32_t fc = 0;
    for (int i = 0; i < nn; i++) if (nodes[i].file >= 0) fc++;
    uint32_t file_off = 64, dir_off = file_off + fc * 32, str_off = dir_off + (uint32_t)no * 24;
    uint32_t data_off = (str_off + (uint32_t)strings.n + 15) & ~15u;
    Buf out = {0};
    buf_reserve(&out, data_off);
    memset(out.p, 0, data_off); out.n = data_off;
    uint32_t *hdr = (uint32_t *)out.p;
    hdr[0] = 0x2066676Du; hdr[1] = 0x5A5A0104u; hdr[2] = 0x10;
    hdr[3] = fc; hdr[4] = fc * 32; hdr[5] = file_off;
    hdr[6] = (uint32_t)no; hdr[7] = (uint32_t)no * 24; hdr[8] = dir_off;
    hdr[9] = (uint32_t)strings.n; hdr[10] = str_off; hdr[11] = 1; hdr[12] = 0x10000;
    /* file index = order of appearance among rows */
    int *file_index = (int *)malloc((size_t)nn * sizeof(int)); uint32_t next_file = 0;
    for (int r = 0; r < no; r++) { int i = order[r]; file_index[i] = nodes[i].file >= 0 ? (int)next_file++ : -1; }
    uint32_t now = (uint32_t)time(NULL);
    for (int r = 0; r < no; r++) {
        int i = order[r]; Node *n = &nodes[i];
        int32_t *row = (int32_t *)(out.p + dir_off + r * 24);
        char path[400]; node_path(nodes, i, path, sizeof(path));
        row[0] = n->is_dir ? (int32_t)path_key(path) : 0;   /* key = ~zlib crc = engine form */
        row[1] = n->parent >= 0 ? row_of[n->parent] : -1;
        row[2] = n->first >= 0 ? row_of[n->first] : -1;
        row[3] = n->next >= 0 ? row_of[n->next] : -1;
        row[4] = (int32_t)n->name_off;
        row[5] = n->is_dir ? -1 : file_index[i];
        if (!n->is_dir) {
            const WEntry *e = &w->e[n->file];
            buf_pad(&out, 16);
            uint32_t off = (uint32_t)out.n;
            buf_put(&out, e->data, e->size);
            uint32_t *rec = (uint32_t *)(out.p + file_off + file_index[i] * 32);
            row = (int32_t *)(out.p + dir_off + r * 24);   /* buffer may have moved */
            hdr = (uint32_t *)out.p;
            rec[0] = (uint32_t)r;
            rec[1] = crc32_of(e->data, e->size) ^ 0xFFFFFFFFu;
            rec[2] = path_key(path);
            rec[3] = rec[4] = e->size;
            rec[5] = now; rec[6] = off; rec[7] = 0x12F700u;
        }
    }
    memcpy(out.p + str_off, strings.p, strings.n);
    int ok = write_file_w(host_path, out.p, out.n);
    free(out.p); free(strings.p); free(order); free(row_of); free(stack); free(file_index); free(nodes);
    return ok;
}

/* ------------------------------------------------------------------ */
/* scenario.ini                                                        */
/* ------------------------------------------------------------------ */
typedef struct Str { uint16_t n; wchar_t *text; } Str;
typedef struct Asset { char archive[64]; char path[192]; } Asset;
typedef struct Scn {
    char id[9];
    int type;                          /* 0 multiplayer, 1 mission */
    wchar_t name[96], desc[256], author[64];
    char base[64];
    char gametypes[16][32]; int ngt;
    Str *strings; int nstrings;
    char audio[64][96]; int naudio_max; /* [Audio] n -> path in the .ma2map, n < 64 */
    Asset *assets; int nassets;
    char resource[80];                 /* ResourceFile written to the map ini (the Base archive) */
    char assets_xbox[96];              /* T:\scenarios\Scn<ID>_assets_<hash>.mgf, or "" */
    wchar_t file[MAX_PATH];
    Mgf mgf;
    uint8_t *world; uint32_t world_size;
    int slot;
    int built;                         /* in a mounted index */
    uint32_t text_base;                /* global text ids SCN_TEXT_GLOBAL + text_base + string index */
    wchar_t shown[200];                /* the row text: Name, or the file name when two of one type share a Name (W-12a) */
    int saved_with;                    /* [Scenario] SavedWith: editor save revision, 0 = missing = older editor (W-13a) */
    wchar_t desc_shown[400];           /* Description plus the "saved with an older editor" note */
    int ready;                         /* [Scenario] Ready (editor's Check passed); 0 = unfinished, not listed (W-15a) */
} Scn;

static Scn *g_scn[SCN_MAX];

/* W-14a: scenario files the game does not list, with a plain reason. A hub visit (or entering Custom
 * Scenarios) shows a note for each new one; rescan() does not reload a skipped file until it changes. */
#define SCN_SKIP_MAX 64
static struct { wchar_t file[MAX_PATH]; FILETIME t; wchar_t why[160]; int noted; } g_skipped[SCN_SKIP_MAX];
static int g_nskipped;
static FILETIME file_time(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a; FILETIME none = {0, 0};
    return GetFileAttributesExW(path, GetFileExInfoStandard, &a) ? a.ftLastWriteTime : none;
}
static void skipped(const wchar_t *path, const wchar_t *why)
{
    int i;
    for (i = 0; i < g_nskipped; i++) if (!_wcsicmp(g_skipped[i].file, path)) break;
    if (i == g_nskipped) { if (g_nskipped >= SCN_SKIP_MAX) return; g_nskipped++; }
    wcsncpy(g_skipped[i].file, path, MAX_PATH - 1); g_skipped[i].file[MAX_PATH - 1] = 0;
    wcsncpy(g_skipped[i].why, why, 159); g_skipped[i].why[159] = 0;
    g_skipped[i].t = file_time(path);
    g_skipped[i].noted = 0;
}
/* 1 when `path` was skipped before and has not been written since. */
static int skipped_known(const wchar_t *path)
{
    for (int i = 0; i < g_nskipped; i++)
        if (!_wcsicmp(g_skipped[i].file, path)) {
            FILETIME t = file_time(path);
            return !CompareFileTime(&t, &g_skipped[i].t);
        }
    return 0;
}
static void skip_notes(void);   /* after editor_note */
static void editor_note(const wchar_t *text);
static int g_nscn;
static uint32_t g_text_next;   /* next free global text id offset */
static volatile LONG g_ready;

static wchar_t *utf8_to_wide(const char *s, int len)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    wchar_t *w = (wchar_t *)calloc((size_t)n + 1, sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, len, w, n);
    return w;
}

static void unescape(wchar_t *s)
{
    wchar_t *o = s;
    for (wchar_t *p = s; *p; p++) {
        if (*p == L'\\' && p[1] == L'n') { *o++ = L'\n'; p++; }
        else *o++ = *p;
    }
    *o = 0;
}

static int parse_ini(Scn *s, const char *text, size_t len)
{
    char section[32] = "";
    const char *p = text, *end = text + len;
    if (len >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;
    int version = 0;
    while (p < end) {
        const char *eol = p; while (eol < end && *eol != '\n') eol++;
        const char *a = p, *b = eol;
        while (a < b && (*a == ' ' || *a == '\t')) a++;
        while (b > a && (b[-1] == '\r' || b[-1] == ' ' || b[-1] == '\t')) b--;
        p = eol + 1;
        if (a >= b || *a == ';' || *a == '#') continue;
        if (*a == '[') {
            const char *c = memchr(a, ']', (size_t)(b - a));
            size_t n = c ? (size_t)(c - a - 1) : 0;
            if (n >= sizeof(section)) n = sizeof(section) - 1;
            memcpy(section, a + 1, n); section[n] = 0;
            continue;
        }
        const char *eq = memchr(a, '=', (size_t)(b - a));
        if (!eq) continue;
        const char *k1 = eq; while (k1 > a && (k1[-1] == ' ' || k1[-1] == '\t')) k1--;
        const char *v0 = eq + 1; while (v0 < b && (*v0 == ' ' || *v0 == '\t')) v0++;
        /* strip a trailing "; comment" from values that are not free text */
        char key[64]; size_t kn = (size_t)(k1 - a); if (kn >= sizeof(key)) kn = sizeof(key) - 1;
        memcpy(key, a, kn); key[kn] = 0;
        int vlen = (int)(b - v0);
        char val[512]; int cut = vlen < (int)sizeof(val) - 1 ? vlen : (int)sizeof(val) - 1;
        memcpy(val, v0, (size_t)cut); val[cut] = 0;
        int free_text = !_stricmp(section, "Strings") || !_stricmp(key, "Name") || !_stricmp(key, "Description") ||
                        !_stricmp(key, "Author") || !_stricmp(key, "Briefing") || !_stricmp(key, "BriefingTitle");
        if (!free_text) { char *sc = strchr(val, ';'); if (sc) { *sc = 0; for (int i = (int)strlen(val); i > 0 && (val[i-1] == ' ' || val[i-1] == '\t'); i--) val[i-1] = 0; } }
        if (!_stricmp(section, "Scenario")) {
            if (!_stricmp(key, "FormatVersion")) version = atoi(val);
            else if (!_stricmp(key, "Id")) { snprintf(s->id, sizeof(s->id), "%.8s", val); for (char *c = s->id; *c; c++) *c = (char)toupper((unsigned char)*c); }
            else if (!_stricmp(key, "Name")) { wchar_t *w = utf8_to_wide(v0, vlen); if (w) { wcsncpy(s->name, w, 95); free(w); } }
            else if (!_stricmp(key, "Description")) { wchar_t *w = utf8_to_wide(v0, vlen); if (w) { unescape(w); wcsncpy(s->desc, w, 255); free(w); } }
            else if (!_stricmp(key, "Author")) { wchar_t *w = utf8_to_wide(v0, vlen); if (w) { wcsncpy(s->author, w, 63); free(w); } }
            else if (!_stricmp(key, "Type")) s->type = !_stricmp(val, "Mission") ? 1 : 0;
            else if (!_stricmp(key, "GameType") && s->ngt < 16) snprintf(s->gametypes[s->ngt++], 32, "%s", val);
            else if (!_stricmp(key, "Base")) snprintf(s->base, sizeof(s->base), "%s", val);
            else if (!_stricmp(key, "SavedWith")) s->saved_with = atoi(val);
            else if (!_stricmp(key, "Ready")) s->ready = atoi(val);
        } else if (!_stricmp(section, "Strings")) {
            int n = atoi(key);
            if (n >= 1 && n <= (int)SCN_TEXT_LOCAL_MAX) {
                s->strings = (Str *)realloc(s->strings, (size_t)(s->nstrings + 1) * sizeof(Str));
                s->strings[s->nstrings].n = (uint16_t)n;
                s->strings[s->nstrings].text = utf8_to_wide(v0, vlen);
                if (s->strings[s->nstrings].text) unescape(s->strings[s->nstrings].text);
                s->nstrings++;
            }
        } else if (!_stricmp(section, "Audio")) {
            int n = atoi(key);
            if (n >= 1 && n < 64) snprintf(s->audio[n], sizeof(s->audio[n]), "%s", val);
        } else if (!_stricmp(section, "Assets")) {
            if (!_stricmp(key, "File")) {
                char *bar = strchr(val, '|');
                if (bar) {
                    *bar = 0;
                    s->assets = (Asset *)realloc(s->assets, (size_t)(s->nassets + 1) * sizeof(Asset));
                    snprintf(s->assets[s->nassets].archive, 64, "%s", val);
                    snprintf(s->assets[s->nassets].path, 192, "%s", bar + 1);
                    s->nassets++;
                }
            }
        }
    }
    return version;
}

/* ------------------------------------------------------------------ */
/* world.wdf: local ids -> global ids                                  */
/* ------------------------------------------------------------------ */
#define SCN_MAX_OBJECTS 3000
/* Returns 1 on success, 0 when the file is not a recognised world (ids stay unmapped, as before), -1 when it
 * holds more than SCN_MAX_OBJECTS objects (the caller skips the scenario instead of crashing the game). */
static int remap_world(Scn *s, uint8_t *w, uint32_t size)
{
    if (size < 20 || *(uint32_t *)w != 0xFF002F00u) return 0;
    uint32_t pos = 20; int texts = 0, cues = 0, missing = 0, objects = 0;
    while (pos + 12 <= size) {
        uint32_t marker = *(uint32_t *)(w + pos), count = *(uint32_t *)(w + pos + 4);
        if (marker != 0xFF012F01u) return 0;
        pos += 12;
        if (++objects > SCN_MAX_OBJECTS) {
            logf_("%s: %d+ objects (max %d), skipped", s->id, objects, SCN_MAX_OBJECTS);
            return -1;
        }
        for (uint32_t i = 0; i < count; i++) {
            if (pos + 16 > size) return 0;
            uint32_t pm = *(uint32_t *)(w + pos), type = *(uint32_t *)(w + pos + 4), psize = *(uint32_t *)(w + pos + 12);
            if (pm != 0xFF022F02u || pos + 16ull + psize > size) return 0;
            if ((type == 3 || type == 6) && psize == 4) {
                uint32_t *v = (uint32_t *)(w + pos + 16);
                if (*v > SCN_TEXT_LOCAL && *v <= SCN_TEXT_LOCAL + SCN_TEXT_LOCAL_MAX) {
                    uint32_t n = *v - SCN_TEXT_LOCAL;
                    int idx = -1;
                    for (int k = 0; k < s->nstrings; k++) if (s->strings[k].n == n) { idx = k; break; }
                    if (idx >= 0) { *v = SCN_TEXT_GLOBAL + s->text_base + (uint32_t)idx; texts++; }
                    else missing++;
                } else if (*v > SCN_CUE_LOCAL && *v <= SCN_CUE_LOCAL + SCN_LOCAL_MAX) {
                    *v = SCN_CUE_GLOBAL + (uint32_t)s->slot * 0x1000u + (*v - SCN_CUE_LOCAL); cues++;
                }
            }
            pos += 16 + psize;
        }
    }
    logf_("%s: %d text ids, %d cue ids made global%s", s->id, texts, cues, missing ? " (some text ids name no [Strings] entry)" : "");
    return 1;
}

/* ------------------------------------------------------------------ */
/* scan + build                                                        */
/* ------------------------------------------------------------------ */
static void scenario_folder(wchar_t *out, size_t cap)
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"RECOMP_SCENARIO_DIR", buf, MAX_PATH);
    if (n && n < MAX_PATH) { wcsncpy(out, buf, cap - 1); out[cap - 1] = 0; return; }
    wchar_t local[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
        wchar_t ini[MAX_PATH];
        swprintf(ini, MAX_PATH, L"%ls\\MechAssault2\\Scenarios.ini", local);
        if (GetPrivateProfileStringW(L"Scenarios", L"Folder", L"", buf, MAX_PATH, ini) && buf[0]) {
            wcsncpy(out, buf, cap - 1); out[cap - 1] = 0; return;
        }
    }
    PWSTR docs = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &docs)) && docs) {
        swprintf(out, cap, L"%ls\\MechAssault 2\\Scenarios", docs);
        CoTaskMemFree(docs);
        return;
    }
    out[0] = 0;
}

static uint8_t *base_archive_bytes(const char *name, uint32_t *size)
{
    char xp[128]; WCHAR host[MAX_PATH];
    snprintf(xp, sizeof(xp), "D:\\%s", name);
    if (!xbox_translate_path(xp, host, MAX_PATH)) return NULL;
    return read_file_w(host, size);
}

/* A retail archive, whole and decompressed; the caller frees it (mgf_free) once done with it. */
static int open_retail(const char *name, Mgf *m)
{
    uint32_t size = 0; uint8_t *raw = base_archive_bytes(name, &size);
    return raw && mgf_load(m, raw, size);
}

/* Host private bytes in MB (the [Assets] build reports its high point). */
static unsigned private_mb(void)
{
    PROCESS_MEMORY_COUNTERS_EX pm = { sizeof pm };
    return K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm) ? (unsigned)(pm.PrivateUsage >> 20) : 0;
}

static int by_archive(const void *a, const void *b)
{
    const Asset *x = *(const Asset *const *)a, *y = *(const Asset *const *)b;
    int c = _stricmp(x->archive, y->archive);
    return c ? c : (x < y ? -1 : x > y);   /* stable: [Assets] order within an archive */
}

static Scn *load_scenario(const wchar_t *path)
{
    uint32_t size = 0;
    uint8_t *raw = read_file_w(path, &size);
    if (!raw) return NULL;
    Scn *s = (Scn *)calloc(1, sizeof(Scn));
    if (!s) { free(raw); return NULL; }
    wcsncpy(s->file, path, MAX_PATH - 1);
    if (!mgf_load(&s->mgf, raw, size)) { logf_("%ls: not an archive", path); skipped(path, L"is not a scenario file"); free(s); return NULL; }
    const MgfEntry *ini = mgf_find(&s->mgf, "scenario.ini");
    const MgfEntry *world = mgf_find(&s->mgf, "world.wdf");
    if (!ini || !world) { logf_("%ls: missing scenario.ini or world.wdf", path); skipped(path, L"is damaged"); mgf_free(&s->mgf); free(s); return NULL; }
    s->ready = 1;   /* files from editors before W-15b have no Ready key */
    int version = parse_ini(s, (const char *)s->mgf.raw + ini->offset, ini->size);
    if (version != 1 || strlen(s->id) != 8 || !s->base[0] || !s->name[0]) {
        logf_("%ls: unsupported (FormatVersion %d, Id '%s', Base '%s')", path, version, s->id, s->base);
        skipped(path, version > 1 ? L"was made by a newer editor" : version < 1 || strlen(s->id) != 8 ? L"is damaged" : L"has no name or base level");
        mgf_free(&s->mgf); free(s); return NULL;
    }
    if (s->type == 0 && !s->ngt) snprintf(s->gametypes[s->ngt++], 32, "TeamDestruction");
    s->world_size = world->size;
    s->world = (uint8_t *)malloc(world->size);
    memcpy(s->world, s->mgf.raw + world->offset, world->size);
    return s;
}

/* T:\scenarios on the host (T:\ itself may not exist yet either); returns 0 if T: has no host path. */
static int scenario_dir(void)
{
    WCHAR dir[MAX_PATH];
    if (!xbox_translate_path("T:\\scenarios\\x", dir, MAX_PATH)) return 0;
    WCHAR *slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0;
    WCHAR parent[MAX_PATH]; wcsncpy(parent, dir, MAX_PATH); WCHAR *s2 = wcsrchr(parent, L'\\');
    if (s2) { *s2 = 0; CreateDirectoryW(parent, NULL); }
    CreateDirectoryW(dir, NULL);
    return 1;
}

/* Resource archive for one scenario: always Base itself (cached and mounted by the level loader
 * like any retail level). [Assets] files go in a small extra archive in T:\scenarios that
 * ma2_scn_level_load mounts next to it. */
static int build_resource(Scn *s)
{
    snprintf(s->resource, sizeof(s->resource), "%s", s->base);
    s->assets_xbox[0] = 0;
    if (!s->nassets) return 1;
    /* name changes whenever the inputs do, so an old copy is never reused; the seed changed when
     * the build stopped dropping files past the 32nd source archive (2026-10-21), so archives the
     * older build left short are made again */
    uint32_t h = 2166136261u ^ 2u;
    for (int i = 0; i < s->nassets; i++) {
        uint32_t a = path_key(s->assets[i].archive), b = path_key(s->assets[i].path);
        h = (h ^ a) * 16777619u; h = (h ^ b) * 16777619u;
    }
    h = (h ^ path_key(s->base)) * 16777619u;
    char xp[96]; WCHAR host[MAX_PATH];
    snprintf(xp, sizeof(xp), "T:\\scenarios\\Scn%s_assets_%08X.mgf", s->id, h);
    if (!xbox_translate_path(xp, host, MAX_PATH)) return 0;
    if (GetFileAttributesW(host) != INVALID_FILE_ATTRIBUTES) {
        snprintf(s->assets_xbox, sizeof(s->assets_xbox), "%s", xp);
        logf_("%s: assets %s (cached)", s->id, xp);
        return 1;
    }
    /* One source archive in memory at a time: the rows grouped by archive, each archive opened,
     * its files copied out, then freed before the next (any number of archives). */
    unsigned mb0 = private_mb(), peak = mb0;
    Mgf base;
    if (!open_retail(s->base, &base)) { logf_("%s: base archive %s not found", s->id, s->base); return 0; }
    const Asset **rows = (const Asset **)malloc((size_t)s->nassets * sizeof *rows);
    uint8_t **copies = (uint8_t **)malloc((size_t)s->nassets * sizeof *copies);
    if (!rows || !copies) abort();
    int nrows = 0, ncopies = 0, missing = 0, in_base = 0, archives = 0;
    for (int i = 0; i < s->nassets; i++) {
        if (mgf_find(&base, s->assets[i].path)) in_base++;   /* the level archive has it */
        else rows[nrows++] = &s->assets[i];
    }
    peak = max(peak, private_mb());
    mgf_free(&base);
    qsort(rows, (size_t)nrows, sizeof *rows, by_archive);
    Writer w = {0};
    for (int i = 0; i < nrows; ) {
        int j = i + 1;
        while (j < nrows && !_stricmp(rows[j]->archive, rows[i]->archive)) j++;
        Mgf src;
        if (!open_retail(rows[i]->archive, &src)) { missing += j - i; i = j; continue; }
        archives++;
        for (; i < j; i++) {
            const MgfEntry *e = mgf_find(&src, rows[i]->path);
            if (!e) { missing++; continue; }
            uint8_t *copy = (uint8_t *)malloc(e->size ? e->size : 1);
            if (!copy) abort();
            memcpy(copy, src.raw + e->offset, e->size);
            copies[ncopies++] = copy;
            w_add(&w, e->path, copy, e->size);
        }
        peak = max(peak, private_mb());
        mgf_free(&src);
    }
    int ok = 1;
    if (w.n) {
        scenario_dir();
        ok = w_write(&w, host);
        if (ok) snprintf(s->assets_xbox, sizeof(s->assets_xbox), "%s", xp);
    }
    logf_("%s: assets %s, %d files from %d archives (%d already in %s, %d missing) -> %s; host memory %u MB, peak %u MB",
          s->id, xp, w.n, archives, in_base, s->base, missing, !w.n ? "none needed" : ok ? "ok" : "WRITE FAILED (playing without them)",
          mb0, peak);
    for (int i = 0; i < ncopies; i++) free(copies[i]);
    free(copies); free(rows); free(w.e);
    return 1;
}

static char g_index_xbox[96];
static int g_nmission;
static int g_focus = -1;   /* slot of the --scenario file (editor Test button), or -1 */
static int g_focus_mission = -1;   /* campaign entry of the --scenario mission */
static Scn *g_mission_scn[SCN_MAX];   /* Custom Scenarios entry -> scenario (the picker's barrel values) */

static char g_camp_name[64] = "scncamp";   /* campaign file stem; changes when a rescan adds missions */

/* Index archive for slots from..g_nscn-1 (rev 0 = boot: everything, plus the MP map set). The
 * custom campaign always lists every mission, so a rescan that adds missions writes a new one
 * under a new name. Returns 1 with g_index_xbox set when there is something to mount. */
static int build_index(int from, int rev)
{
    Writer w = {0};
    Buf set = {0}; Buf inis[SCN_MAX]; memset(inis, 0, sizeof(inis));
    Buf camp = {0};
    Scn *missions[SCN_MAX];
    buf_str(&set, "[Missions]\r\n");
    int mp = 0, new_missions = 0;
    for (int i = from; i < g_nscn; i++) {
        Scn *s = g_scn[i];
        if (!build_resource(s)) continue;
        if (remap_world(s, s->world, s->world_size) < 0) { skipped(s->file, L"has more than 3,000 objects"); continue; }
        s->built = 1;
        char path[160];
        if (s->type == 0) {
            Buf *ini = &inis[i];
            char line[256];
            buf_str(ini, "[MissionVersionInfo]\r\nVersionNumber=1\r\n\r\n[Mission]\r\nPackageNumber = 0\r\n");
            snprintf(line, sizeof(line), "WorldFile = Scn%s.wdf\r\nResourceFile = %s\r\nMapName = %u\r\nMapDescription = %u\r\n",
                     s->id, s->resource, SCN_NAME_BASE + 2u * (uint32_t)s->slot, SCN_NAME_BASE + 2u * (uint32_t)s->slot + 1u);
            buf_str(ini, line);
            buf_str(ini, "UseStandardMechResources = false\r\n");
            for (int g = 0; g < s->ngt; g++) { snprintf(line, sizeof(line), "GameType = %s\r\n", s->gametypes[g]); buf_str(ini, line); }
            snprintf(path, sizeof(path), "Multiplay/Scn%s.ini", s->id);
            w_add(&w, path, ini->p, (uint32_t)ini->n);
            snprintf(path, sizeof(path), "Multiplay/Scn%s.wdf", s->id);
            w_add(&w, path, s->world, s->world_size);
            snprintf(line, sizeof(line), "Mission = Multiplay/Scn%s.ini\r\n", s->id);
            buf_str(&set, line);
            mp++;
        } else {
            snprintf(path, sizeof(path), "Missions/Scn%s.wdf", s->id);
            w_add(&w, path, s->world, s->world_size);
            new_missions++;
        }
        /* the scenario's own files (terrain pictures, pathfinding, ...) at their own paths */
        for (int f = 0; f < s->mgf.n; f++) {
            const MgfEntry *e = &s->mgf.e[f];
            if (!_stricmp(e->path, "scenario.ini") || !_stricmp(e->path, "world.wdf") || !_stricmp(e->path, "templates.wdf") ||
                istarts(e->path, "editor/") || istarts(e->path, "audio/") || !_stricmp(e->path, "preview.tif")) continue;
            w_add(&w, e->path, s->mgf.raw + e->offset, e->size);
        }
        logf_("%s '%ls' (%s, base %s, %d strings) slot %d", s->id, s->name, s->type ? "mission" : "multiplayer", s->resource, s->nstrings, s->slot);
    }
    if (mp && rev == 0) w_add(&w, "Multiplay/MissionSet63.ini", set.p, (uint32_t)set.n);
    if (new_missions) {
        /* Custom missions form one campaign in the retail format (Missions\campaign0.ini). Custom
         * Scenarios mode points the campaign path format at it (ma2_scn_custom_enter). */
        int n = 0;
        g_focus_mission = -1;
        for (int i = 0; i < g_nscn; i++)
            if (g_scn[i]->type == 1 && g_scn[i]->built) { if (i == g_focus) g_focus_mission = n; missions[n++] = g_scn[i]; }
        g_nmission = n;
        for (int m = 0; m < n; m++) g_mission_scn[m] = missions[m];
        buf_str(&camp, "[fileinfo]\r\nfile_version = 1\r\n\r\n[StartCampaign]\r\ntype = INIT\r\nnext = SCN000\r\n\r\n");
        for (int m = 0; m < g_nmission; m++) {
            const Scn *s = missions[m];
            char line[320], next[16];
            if (m + 1 < g_nmission) snprintf(next, sizeof(next), "SCN%03d", m + 1); else snprintf(next, sizeof(next), "EndCampaign");
            snprintf(line, sizeof(line),
                     "[SCN%03d]\r\ntype = LEVEL\r\nfile = missions\\Scn%s.wdf\r\nnext = %s\r\ntitleid = %u\r\ndescriptionid = %u\r\nreturntolobby = true\r\n\r\n",
                     m, s->id, next, SCN_NAME_BASE + 2u * (uint32_t)s->slot, SCN_NAME_BASE + 2u * (uint32_t)s->slot + 1u);
            buf_str(&camp, line);
        }
        buf_str(&camp, "[EndCampaign]\r\ntype = END\r\n\r\n\r\n\r\n");
        if (rev) snprintf(g_camp_name, sizeof(g_camp_name), "scnr%02d_", rev);
        char path[96]; snprintf(path, sizeof(path), "Missions/%s0.ini", g_camp_name);
        w_add(&w, path, camp.p, (uint32_t)camp.n);
    }
    int ok = 0;
    g_index_xbox[0] = 0;
    if (w.n) {
        WCHAR dir[MAX_PATH], host[MAX_PATH];
        if (scenario_dir() && xbox_translate_path("T:\\scenarios\\x", dir, MAX_PATH)) {
            WCHAR *slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0;
            static unsigned stamp;
            if (rev == 0) {
                /* A new name per boot: the game keeps the archive open, so last run's may still be
                 * locked by a second instance. Old ones are removed when they can be. */
                WIN32_FIND_DATAW fd; WCHAR pattern[MAX_PATH];
                swprintf(pattern, MAX_PATH, L"%ls\\index_*.mgf", dir);
                HANDLE fh = FindFirstFileW(pattern, &fd);
                if (fh != INVALID_HANDLE_VALUE) {
                    do { WCHAR old[MAX_PATH]; swprintf(old, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName); DeleteFileW(old); } while (FindNextFileW(fh, &fd));
                    FindClose(fh);
                }
                stamp = (unsigned)(GetTickCount() ^ GetCurrentProcessId()) & 0xFFFFFF;
            }
            if (rev == 0) snprintf(g_index_xbox, sizeof(g_index_xbox), "T:\\scenarios\\index_%06X.mgf", stamp);
            else snprintf(g_index_xbox, sizeof(g_index_xbox), "T:\\scenarios\\index_%06X_%02d.mgf", stamp, rev);
            if (!xbox_translate_path(g_index_xbox, host, MAX_PATH) || !w_write(&w, host)) {
                logf_("could not write %s", g_index_xbox); g_index_xbox[0] = 0;
            } else {
                logf_("index %s: %d files, %d new multiplayer maps, %d missions in all", g_index_xbox, w.n, mp, g_nmission);
                ok = 1;
            }
        }
    }
    free(w.e); free(set.p); free(camp.p);
    for (int i = 0; i < SCN_MAX; i++) free(inis[i].p);
    return ok;
}


static void add_scenario(const wchar_t *path, int focus)
{
    if (g_nscn >= SCN_MAX) { if (!focus) skipped(path, L"is one of more than 200 scenarios"); return; }
    Scn *s = load_scenario(path);
    if (!s) return;
    if (!s->ready && !focus) {   /* D-4 (a): hide unfinished scenarios; the editor's Test still loads its file */
        logf_("%ls: Ready = 0, not listed", path);
        skipped(path, L"is not finished (the editor's Check scenario lists what is missing)");
        mgf_free(&s->mgf); free(s);
        return;
    }
    for (int i = 0; i < g_nscn; i++)
        if (!strcmp(g_scn[i]->id, s->id)) {
            if (!focus) {
                logf_("%ls: duplicate Id %s, skipped", path, s->id);
                const wchar_t *other = wcsrchr(g_scn[i]->file, L'\\'); other = other ? other + 1 : g_scn[i]->file;
                wchar_t why[160];
                swprintf(why, 160, L"is a copy of %.60ls (open it in the Scenario Editor and use File > Save as)", other);
                skipped(path, why);
                return;
            }
            g_focus = i;   /* the Test file is also in the folder: the folder copy is loaded already */
            logf_("test scenario %s is already loaded from the folder (slot %d)", s->id, i);
            return;
        }
    s->slot = g_nscn;
    s->text_base = g_text_next; g_text_next += (uint32_t)s->nstrings;
    if (focus) g_focus = g_nscn;
    g_scn[g_nscn++] = s;
}

/* "MechAssault 2.exe --scenario <file.ma2map>" (the editor's Test button), or RECOMP_SCENARIO_FILE. */
static int test_file(wchar_t *out, size_t cap)
{
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"RECOMP_SCENARIO_FILE", env, MAX_PATH) && env[0]) { wcsncpy(out, env, cap - 1); out[cap - 1] = 0; return 1; }
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int found = 0;
    for (int i = 1; argv && i + 1 < argc; i++)
        if (!_wcsicmp(argv[i], L"--scenario")) { wcsncpy(out, argv[i + 1], cap - 1); out[cap - 1] = 0; found = 1; break; }
    if (argv) LocalFree(argv);
    return found;
}

/* ------------------------------------------------------------------ */
/* editor Test: straight into the scenario                             */
/* ------------------------------------------------------------------ */
/* The Test button also passes --mode <GameType> --bots <red>v<blue> --skill <0-4> for a
 * multiplayer map (docs/editor.md). The menus are then walked the way a player would (ma2_scn_autostart,
 * one step per menu as each appears): Press Start -> hub -> Multiplayer -> Local -> New Game with
 * the mode and map selected -> Briefing Room -> match. A mission takes the hub's Custom Scenarios
 * item; its picker already starts on the Test file. The bot counts and skill hold for that launch
 * only (bots/ma2_bots_ui.c ma2_bots_test_config). */
static const struct { const char *name; int gametype; } k_modes[] = {   /* sub_001661C1 */
    { "TeamDestruction", 3 }, { "Destruction", 0xA }, { "CTF", 0xE }, { "BaseWar", 0x10 },
    { "SnatchIt", 0x11 }, { "CheckIt", 0xF }, { "TeamLastManStanding", 1 }, { "TeamLMS", 1 },
    { "LastManStanding", 9 }, { "LMS", 9 }, { "NotIt", 4 }, { "Grinder", 6 },
};
static int mode_number(const char *name)
{
    for (size_t i = 0; i < sizeof k_modes / sizeof *k_modes; i++) if (!_stricmp(k_modes[i].name, name)) return k_modes[i].gametype;
    return -1;
}
static int team_type(int gt) { return (gt > 0 && gt <= 3) || (gt > 0xA && gt <= 0x11); }   /* sub_16616C */

enum { AUTO_OFF, AUTO_RUN, AUTO_WAIT_MATCH };
static struct {
    int state;
    int gametype;                       /* multiplayer: the game's number for the mode */
    char mode[32];
    int red, blue, skill;               /* -1: not given */
    uint32_t vt, object;                /* menu the last step acted on */
    unsigned frames;                    /* frames seen on the current menu */
    DWORD since;                        /* last progress */
    int ng_phase;                       /* New Game: 0 mode, 1 map, 2 settle, 3 started */
    unsigned ng_wait;
    uint32_t acted;                     /* the menu object the last press was made on */
    DWORD acted_at; int tries;          /* when, and how many repeats of it */
    unsigned acted_frames;              /* menu frames on it since */
    int browse;                         /* --browse: walk to Custom Scenarios and stop there */
} g_auto = { AUTO_OFF, -1, "", -1, -1, -1 };

void ma2_bots_test_config(int red, int blue, int ffa, int skill);   /* bots/ma2_bots_ui.c */
void ma2_bots_test_release(int now);

static void auto_log(const char *fmt, ...)
{
    char line[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    xbox_log_redact(line, sizeof line);
    fputs("[SCENARIO] test autostart: ", stderr); fputs(line, stderr); fputc('\n', stderr); fflush(stderr);
}

static void test_args(void)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; i++) {
        char v[64]; WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, v, sizeof v, NULL, NULL); v[63] = 0;
        if (!_wcsicmp(argv[i], L"--mode")) snprintf(g_auto.mode, sizeof g_auto.mode, "%s", v);
        else if (!_wcsicmp(argv[i], L"--skill")) g_auto.skill = atoi(v);
        else if (!_wcsicmp(argv[i], L"--bots")) {   /* "<red>v<blue>" (editor) or "<red>,<blue>" */
            char *sep = strpbrk(v, "vVxX,");
            if (sep) { *sep = 0; g_auto.red = atoi(v); g_auto.blue = atoi(sep + 1); }
        }
    }
    for (int i = 1; argv && i < argc; i++) if (!_wcsicmp(argv[i], L"--browse")) g_auto.browse = 1;
    if (argv) LocalFree(argv);
}

/* After scan(): arm the walk for the loaded Test file. */
static void auto_arm(void)
{
    const char *off = getenv("RECOMP_SCENARIO_AUTOSTART");
    if (g_focus < 0 || (off && !strcmp(off, "0"))) return;
    const Scn *s = g_scn[g_focus];
    if (!s->built) { auto_log("off (%s was not built)", s->id); return; }
    test_args();
    if (s->type == 0) {
        /* --mode, if the map has it; otherwise the map's first mode */
        int pick = -1;
        for (int g = 0; g < s->ngt && pick < 0; g++) if (g_auto.mode[0] && !_stricmp(s->gametypes[g], g_auto.mode)) pick = g;
        /* aliases (LMS, TeamLMS, ...): match the map's mode by the game's number */
        int want = g_auto.mode[0] ? mode_number(g_auto.mode) : -1;
        for (int g = 0; g < s->ngt && pick < 0 && want >= 0; g++) if (mode_number(s->gametypes[g]) == want) pick = g;
        if (pick < 0 && g_auto.mode[0]) auto_log("mode %s is not one of %s's modes, using %s", g_auto.mode, s->id, s->gametypes[0]);
        if (pick < 0) pick = 0;
        snprintf(g_auto.mode, sizeof g_auto.mode, "%s", s->gametypes[pick]);
        g_auto.gametype = mode_number(g_auto.mode);
        if (g_auto.gametype < 0) { auto_log("off (unknown mode %s)", g_auto.mode); return; }
        int ffa = g_auto.red < 0 && g_auto.blue < 0 ? -1 : (g_auto.red > 0 ? g_auto.red : 0) + (g_auto.blue > 0 ? g_auto.blue : 0);
        if (g_auto.red >= 0 || g_auto.blue >= 0 || g_auto.skill >= 0)
            ma2_bots_test_config(g_auto.red, g_auto.blue, ffa, g_auto.skill > 4 ? 4 : g_auto.skill);
        auto_log("armed: %s '%ls' as %s (type %d), bots red %d blue %d (free-for-all %d), skill %d (this launch only)",
                 s->id, s->name, g_auto.mode, g_auto.gametype, g_auto.red, g_auto.blue, ffa, g_auto.skill);
    } else {
        if (g_focus_mission < 0) { auto_log("off (%s is not in the custom campaign)", s->id); return; }
        auto_log("armed: mission %s '%ls' (Custom Scenarios entry %d)", s->id, s->name, g_focus_mission);
    }
    g_auto.state = AUTO_RUN;
    g_auto.since = GetTickCount();
}

/* W-12a: two scenarios of one type with the same Name would be identical rows (and Test of the
 * second could start the first, auto_newgame picks by the row text). Such rows show the file name
 * ("New Mission (2)") or "Name (file name)". */
/* SavedWith 2 = the editor writes the asset closure with .mtb references and the MABuilding code files;
 * older files can show pink markers in place of buildings until re-saved. FormatVersion stays 1. */
#define SCN_SAVE_REV 2
static void name_shown(void)
{
    for (int i = 0; i < g_nscn; i++) {
        Scn *s = g_scn[i]; int clash = 0;
        for (int j = 0; j < g_nscn && !clash; j++)
            clash = j != i && g_scn[j]->type == s->type && !_wcsicmp(g_scn[j]->name, s->name);
        const wchar_t *b = wcsrchr(s->file, L'\\'); b = b ? b + 1 : s->file;
        wchar_t stem[80]; wcsncpy(stem, b, 79); stem[79] = 0;
        wchar_t *dot = wcsrchr(stem, L'.'); if (dot) *dot = 0;
        if (!clash) swprintf(s->shown, 200, L"%ls", s->name);
        else if (!_wcsnicmp(stem, s->name, wcslen(s->name))) swprintf(s->shown, 200, L"%ls", stem);  /* "New Mission (2)" */
        else swprintf(s->shown, 200, L"%.40ls (%.40ls)", s->name, stem);                              /* "City Fight (New Mission (3))" */
        if (clash) logf_("%s '%ls' shares its name: listed as '%ls'", s->id, s->name, s->shown);
        if (s->saved_with < SCN_SAVE_REV)
            swprintf(s->desc_shown, 400, L"%.250ls%ls(Older editor file: if you see pink markers, re-save it in the Scenario Editor.)",
                     s->desc, s->desc[0] ? L"\n" : L"");
        else swprintf(s->desc_shown, 400, L"%ls", s->desc);
    }
}

static void scan(void)
{
    const char *off = getenv("RECOMP_SCENARIOS");
    if (off && !strcmp(off, "0")) { logf_("disabled (RECOMP_SCENARIOS=0)"); return; }
    wchar_t folder[MAX_PATH]; scenario_folder(folder, MAX_PATH);
    if (!folder[0]) logf_("no scenario folder");
    WIN32_FIND_DATAW fd; wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*.ma2map", folder);
    HANDLE h = folder[0] ? FindFirstFileW(pattern, &fd) : INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%ls\\%ls", folder, fd.cFileName);
            add_scenario(path, 0);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    logf_("folder %ls: %d scenario(s)", folder, g_nscn);
    wchar_t test[MAX_PATH];
    if (test_file(test, MAX_PATH)) {
        add_scenario(test, 1);
        if (g_focus >= 0) logf_("test scenario %ls -> %s '%ls' (slot %d)", test, g_scn[g_focus]->id, g_scn[g_focus]->name, g_focus);
        else logf_("test scenario %ls could not be loaded", test);
    }
    if (!g_nscn) return;
    name_shown();
    build_index(0, 0);
    if (g_index_xbox[0]) auto_arm();
}

/* ------------------------------------------------------------------ */
/* guest calls                                                         */
/* ------------------------------------------------------------------ */
typedef struct Regs { uint32_t a, c, d, b, s, i, sp, bp, seh; } Regs;
static Regs regs_save(void) { Regs r = { eax, ecx, edx, ebx, esi, edi, esp, g_ebp, g_seh_ebp }; return r; }
static void regs_restore(const Regs *r)
{
    eax = r->a; ecx = r->c; edx = r->d; ebx = r->b; esi = r->s; edi = r->i; esp = r->sp; g_ebp = r->bp; g_seh_ebp = r->seh;
}

/* Mount an archive the way the boot sequence mounts Common.mgf / Templates.mgf: std::string from a
 * C string (sub_0001A060), mount (sub_00025B70, eax=&string, edi=fs, two zero args, callee pops 8),
 * destroy (sub_00019D40). Runs on the game thread. */
static uint32_t mount_archive(const char *xbox_path, uint32_t fs)
{
    Regs r = regs_save();
    uint32_t frame = (esp - 0x100) & ~15u;
    uint32_t text = frame + 0x40, str = frame + 0x20;
    for (int i = 0; ; i++) { MEM8(text + i) = (uint8_t)xbox_path[i]; if (!xbox_path[i]) break; }
    esp = frame;
    esi = text; eax = str;
    PUSH32(esp, 0x00012F81u); RECOMP_ABI_CALL(0x0001A060u, sub_0001A060);
    esp = frame; edi = fs;
    PUSH32(esp, 0); PUSH32(esp, 0);
    eax = str;
    PUSH32(esp, 0x00012F8Cu); RECOMP_ABI_CALL(0x00025B70u, sub_00025B70);
    uint32_t mounted = eax & 0xFF;
    esp = frame; ecx = str;
    PUSH32(esp, 0x00012F95u); RECOMP_ABI_CALL(0x00019D40u, sub_00019D40);
    regs_restore(&r);
    logf_("mounted %s on fs %08X -> %u", xbox_path, fs, mounted);
    return mounted;
}

/* Unmount by name the way the level unload (sub_000171E0) drops a level archive: sub_00025C90
 * (stdcall fs, &string; callee pops 8), then on success sub_00025D10 (eax=fs), which resets the
 * file system's lookup state. */
static uint32_t unmount_archive(const char *xbox_path, uint32_t fs)
{
    Regs r = regs_save();
    uint32_t frame = (esp - 0x100) & ~15u;
    uint32_t text = frame + 0x40, str = frame + 0x20;
    for (int i = 0; ; i++) { MEM8(text + i) = (uint8_t)xbox_path[i]; if (!xbox_path[i]) break; }
    esp = frame;
    esi = text; eax = str;
    PUSH32(esp, 0x00017972u); RECOMP_ABI_CALL(0x0001A060u, sub_0001A060);
    esp = frame;
    PUSH32(esp, str); PUSH32(esp, fs);
    PUSH32(esp, 0x00017A0Du); RECOMP_ABI_CALL(0x00025C90u, sub_00025C90);
    uint32_t done = eax & 0xFF;
    if (done) {
        esp = frame; eax = fs;
        PUSH32(esp, 0x00017A1Cu); RECOMP_ABI_CALL(0x00025D10u, sub_00025D10);
    }
    esp = frame; ecx = str;
    PUSH32(esp, 0x00017A1Cu); RECOMP_ABI_CALL(0x00019D40u, sub_00019D40);
    regs_restore(&r);
    logf_("unmounted %s from fs %08X -> %u", xbox_path, fs, done);
    return done;
}

/* [Assets] archive of the level being played. The level loader (sub_00013550) mounts the Base
 * archive itself; at the end of its mount block (loc_000138A0, reached whether or not it mounted)
 * the scenario whose world the level descriptor names gets its assets archive mounted on the same
 * file system. The level unload (sub_000171E0, after its own unmount, loc_000178C9) drops it, so a
 * retail level never sees it. */
#define VA_LEVEL_DESC 0x00524A60u   /* +4: world path (char *) */
#define VA_FS         0x0050BAC8u
static char g_level_assets[96];
static uint32_t g_level_fs;

static const Scn *level_scenario(void)
{
    uint32_t desc = MEM32(VA_LEVEL_DESC), world = desc ? MEM32(desc + 4) : 0;
    if (!world) return NULL;
    char path[0x104]; int n = 0;
    for (; n < 0x103; n++) { path[n] = (char)MEM8(world + n); if (!path[n]) break; }
    path[n] = 0;
    const char *name = strrchr(path, '\\'), *fwd = strrchr(path, '/');
    if (fwd && (!name || fwd > name)) name = fwd;
    name = name ? name + 1 : path;
    if (!istarts(name, "scn") || strlen(name) < 11) return NULL;
    for (int i = 0; i < g_nscn; i++) if (!_strnicmp(name + 3, g_scn[i]->id, 8)) return g_scn[i];
    return NULL;
}

static void quick_level_loaded(int scenario);   /* --quick-test, below */
static void quick_level_unloaded(void);

static void drop_level_assets(void)
{
    if (!g_level_assets[0]) return;
    unmount_archive(g_level_assets, g_level_fs);
    g_level_assets[0] = 0; g_level_fs = 0;
}

void ma2_scn_level_unload(void)
{
    quick_level_unloaded();
    drop_level_assets();
}

void ma2_scn_level_load(void)
{
    if (!g_ready) return;
    const Scn *s = level_scenario();
    quick_level_loaded(s != NULL);
    const char *want = s && s->assets_xbox[0] ? s->assets_xbox : "";
    uint32_t fs = MEM32(VA_FS);
    if (g_level_assets[0] && (_stricmp(g_level_assets, want) || g_level_fs != fs)) drop_level_assets();
    if (!want[0] || g_level_assets[0] || !fs) return;
    if (mount_archive(want, fs)) { snprintf(g_level_assets, sizeof(g_level_assets), "%s", want); g_level_fs = fs; }
    else logf_("%s: assets archive did not mount", s->id);
}

/* Called from the boot mount sequence (recomp_0000.c, after Common.mgf), with the file system
 * object the original mounts use. */
void ma2_scn_mount(uint32_t fs)
{
    if (InterlockedCompareExchange(&g_ready, 1, 0) != 0) return;
    scan();
    if (!g_index_xbox[0]) return;
    if (!fs) { logf_("mount skipped: no file system object"); return; }
    mount_archive(g_index_xbox, fs);
}

/* ------------------------------------------------------------------ */
/* rescan: scenarios saved while the game runs                         */
/* ------------------------------------------------------------------ */
/* On the hub and the Multiplayer menu (ma2_scn_menu_frame), the folder is checked for .ma2map
 * files that were not there at boot. New ones get a slot, a delta index archive that is mounted
 * like the boot one, and -- multiplayer maps -- a call to the map-ini loader the boot map-set
 * pass uses for each "Mission =" line: sub_0016321D(game, path) (stdcall, 2 args; game =
 * [0x50C958], file system = game+0x28). New missions get a new campaign file listing all of them.
 * A changed or deleted scenario keeps its boot version until the game restarts (the editor's
 * Test button restarts it anyway). */
#define VA_GAME_OBJECT 0x0050C958u
static int g_rev;

static int loaded_path(const wchar_t *path)
{
    for (int i = 0; i < g_nscn; i++) if (!_wcsicmp(g_scn[i]->file, path)) return 1;
    return 0;
}

static void add_map_ini(uint32_t game, const char *path)
{
    Regs r = regs_save();
    uint32_t frame = (esp - 0x100) & ~15u, text = frame + 0x40;
    for (int i = 0; ; i++) { MEM8(text + i) = (uint8_t)path[i]; if (!path[i]) break; }
    esp = frame;
    PUSH32(esp, text); PUSH32(esp, game);
    PUSH32(esp, 0x001631EBu); RECOMP_ABI_CALL(0x0016321Du, sub_0016321D);
    uint32_t ok = eax & 0xFF;
    regs_restore(&r);
    logf_("map list: added %s -> %u", path, ok);
}

static void rescan(void)
{
    const char *off = getenv("RECOMP_SCENARIOS");
    if ((off && !strcmp(off, "0")) || g_rev >= 60) return;
    wchar_t folder[MAX_PATH]; scenario_folder(folder, MAX_PATH);
    if (!folder[0]) return;
    WIN32_FIND_DATAW fd; wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*.ma2map", folder);
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int from = g_nscn;
    do {
        wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%ls\\%ls", folder, fd.cFileName);
        if (!loaded_path(path) && !skipped_known(path)) add_scenario(path, 0);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (g_nscn == from) return;
    uint32_t game = MEM32(VA_GAME_OBJECT), fs = game ? MEM32(game + 0x28) : 0;
    if (!fs) { logf_("rescan: no file system object yet"); return; }
    name_shown();
    int rev = ++g_rev;
    if (!build_index(from, rev)) return;
    if (!mount_archive(g_index_xbox, fs)) return;
    for (int i = from; i < g_nscn; i++)
        if (g_scn[i]->type == 0 && g_scn[i]->built) {
            char ini[64]; snprintf(ini, sizeof(ini), "Multiplay/Scn%s.ini", g_scn[i]->id);
            add_map_ini(game, ini);
        }
    logf_("rescan: %d new scenario(s)", g_nscn - from);
}

/* Level resource path fix-up (recomp_0037.c, after "%s%s.mgf"): archives named Scn* live in
 * T:\scenarios. `buf` is the guest char buffer (0x104 bytes). */
void ma2_scn_resource_path(uint32_t buf)
{
    char path[0x104]; int n = 0;
    for (; n < 0x103; n++) { path[n] = (char)MEM8(buf + n); if (!path[n]) break; }
    path[n] = 0;
    const char *name = strrchr(path, '\\'); name = name ? name + 1 : path;
    if (!istarts(name, "scn")) return;
    /* "<root>Scn<ID>.mgf" is what a level asks for when its world is Scn<ID>.wdf (missions have
     * no map ini, and the MP lobby probes it too): send it to the scenario's real resource. */
    const Scn *s = NULL;
    if (strlen(name) >= 11)
        for (int i = 0; i < g_nscn; i++) if (!_strnicmp(name + 3, g_scn[i]->id, 8)) { s = g_scn[i]; break; }
    char out[0x104];
    int len;
    if (s && !istarts(s->resource, "scn")) {
        size_t root = (size_t)(name - path);
        len = snprintf(out, sizeof(out), "%.*s%s", (int)root, path, s->resource);
    } else if (s && name[11] == '.') {
        len = snprintf(out, sizeof(out), "T:\\scenarios\\%s", s->resource);
    } else {
        len = snprintf(out, sizeof(out), "T:\\scenarios\\%s", name);
    }
    if (len <= 0 || len >= 0x104) return;
    for (int i = 0; i <= len; i++) MEM8(buf + i) = (uint8_t)out[i];
    logf_("resource %s -> %s", path, out);
}

/* ------------------------------------------------------------------ */
/* Custom Scenarios mode (main menu, the old Game Demos slot)          */
/* ------------------------------------------------------------------ */
/* The hub's action-8 button (hub_main_demos.ggf, relabelled by the asset patch) opens the Campaign
 * Picker (activatecommand MAAutoMenuCampaignPick). recomp_0051.c sub_001F3BCC calls
 * ma2_scn_custom_enter() there instead of launching d:\demo\cdx.xbe. While the mode is on:
 *   - the campaign path format [0x4B2910] ("\missions\campaign%01d") points at
 *     "\missions\scncamp%01d", so the only campaign is the generated one and the picker
 *     auto-selects it (one campaign -> no list);
 *   - the profile save names (std::strings 0x50E838 "MASave.sav", 0x50E844 "MASaveGame.wdf")
 *     become "MAScns.sav" / "MAScnsGame.wdf" (same length, edited in place), so nothing a
 *     custom mission saves reaches the real campaign profile or checkpoint;
 *   - every player's campaign fields (profile +0x60..+0x93: per difficulty current/highest
 *     destination and more, +0x90 campaign index) are snapshot and every custom mission unlocked.
 *   - the campaign intro is never chosen: the mission start (sub_002439B7) plays SPIntro when the
 *     profile's campaign index (+0x90, zeroed above) is 0 and the "intro seen" byte [0x50CA4A] is
 *     0; that byte is held at 1 here (sub_00224E15 clears it again on its way) and restored on leave,
 *     so the real campaign still gets its intro.
 * The hub's own form build (sub_001F3263) calls ma2_scn_custom_leave(), restoring all of it. */
#define VA_FRONTEND        0x00524A6Cu   /* sub_00018EB0 */
#define VA_INTRO_SEEN      0x0050CA4Au
#define VA_CAMPAIGN_FMT    0x004B2910u
#define VA_SAVE_NAME       0x0050E838u
#define VA_CHECKPOINT_NAME 0x0050E844u
#define PROFILE_SIZE       0x11Cu
#define PROFILE_SNAP_OFF   0x60u
#define PROFILE_SNAP_LEN   0x40u   /* +0x60..+0x9F: destinations, +0x90 difficulty, +0x98 checkpoint (ma2_probe_campaign) */
#define SCN_UI_BASE        0x00FFFF00u   /* fixed UI strings */

static int g_custom;
static uint8_t g_intro_seen;
static uint8_t g_prof_snap[4][PROFILE_SNAP_LEN];
static uint32_t g_fmt_saved, g_fmt_guest;
extern uint32_t xbox_HeapAlloc(uint32_t size, uint32_t align);

static int swap_string(uint32_t obj, const char *from, const char *to)
{
    uint32_t begin = MEM32(obj), end = MEM32(obj + 4);
    size_t n = strlen(from);
    if (!begin || end - begin != n || strlen(to) != n) return 0;
    for (size_t i = 0; i < n; i++) if ((char)MEM8(begin + (uint32_t)i) != from[i]) return 0;
    for (size_t i = 0; i < n; i++) MEM8(begin + (uint32_t)i) = (uint8_t)to[i];
    return 1;
}

static uint32_t profiles(void)
{
    uint32_t fe = MEM32(VA_FRONTEND);
    return fe ? MEM32(fe + 0x24) : 0;
}

int ma2_scn_mission_count(void) { return g_nmission; }
int ma2_scn_custom_active(void) { return g_custom; }
/* The level being played is a scenario (.ma2map: an editor-made map or a copy of a retail one). */
int ma2_scn_level_custom(void) { return g_ready && level_scenario() != NULL; }

static int g_empty_prompt;             /* Custom Scenarios with no missions: ask about the editor (editor_frame) */

/* Returns 0 when the hub should not open the mission picker (no missions: the editor prompt instead). */
int ma2_scn_custom_enter(void)
{
    skip_notes();
    if (!g_nmission) { g_empty_prompt = 1; logf_("custom mode: no missions, offering the editor"); return 0; }
    /* Still on from the last visit (the hub's form build did not switch it off, e.g. back from the
     * in-game editor through Esc): a rescan may have added missions, so point at the current
     * campaign file and unlock all of them again; the snapshots and save names stay as they are. */
    const int again = g_custom;
    uint32_t prof = profiles();
    if (!prof) { logf_("custom mode: no profile table"); return 1; }
    {   /* "\missions\<campaign stem>%01d" in guest memory; a rescan may have renamed the campaign */
        static char made[64];
        if (!g_fmt_guest || strcmp(made, g_camp_name)) {
            char fmt[96];
            snprintf(fmt, sizeof(fmt), "\\missions\\%s%%01d", g_camp_name);
            size_t n = strlen(fmt) + 1;
            g_fmt_guest = xbox_HeapAlloc((uint32_t)n, 16);
            if (!g_fmt_guest) return 1;
            for (size_t i = 0; i < n; i++) MEM8(g_fmt_guest + (uint32_t)i) = (uint8_t)fmt[i];
            snprintf(made, sizeof(made), "%s", g_camp_name);
        }
    }
    for (uint32_t p = 0; p < 4; p++) {
        uint32_t rec = prof + p * PROFILE_SIZE;
        if (!again) for (uint32_t i = 0; i < PROFILE_SNAP_LEN; i++) g_prof_snap[p][i] = MEM8(rec + PROFILE_SNAP_OFF + i);
        for (uint32_t d = 0; d < 3; d++) {
            MEM32(rec + 0x60 + d * 12) = 0;                          /* current destination */
            /* highest unlocked; at least 1, or a one-mission campaign skips the picker (and the
             * Editor button on it) and starts the mission at once */
            MEM32(rec + 0x64 + d * 12) = (uint32_t)(g_nmission > 1 ? g_nmission - 1 : 1);
        }
        MEM32(rec + 0x90) = 0;
    }
    if (again) {
        MEM32(VA_CAMPAIGN_FMT) = g_fmt_guest;
        logf_("custom mode refreshed: %d missions (%s)", g_nmission, g_camp_name);
        return 1;
    }
    int a = swap_string(VA_SAVE_NAME, "MASave.sav", "MAScns.sav");
    int b = swap_string(VA_CHECKPOINT_NAME, "MASaveGame.wdf", "MAScnsGame.wdf");
    g_fmt_saved = MEM32(VA_CAMPAIGN_FMT);
    MEM32(VA_CAMPAIGN_FMT) = g_fmt_guest;
    g_intro_seen = MEM8(VA_INTRO_SEEN);
    MEM8(VA_INTRO_SEEN) = 1;
    g_custom = 1;
    logf_("custom mode on: %d missions, save names %s/%s", g_nmission, a ? "swapped" : "UNCHANGED", b ? "swapped" : "UNCHANGED");
    return 1;
}

void ma2_scn_custom_leave(void)
{
    if (!g_custom) return;
    uint32_t prof = profiles();
    if (prof)
        for (uint32_t p = 0; p < 4; p++)
            for (uint32_t i = 0; i < PROFILE_SNAP_LEN; i++) MEM8(prof + p * PROFILE_SIZE + PROFILE_SNAP_OFF + i) = g_prof_snap[p][i];
    swap_string(VA_SAVE_NAME, "MAScns.sav", "MASave.sav");
    swap_string(VA_CHECKPOINT_NAME, "MAScnsGame.wdf", "MASaveGame.wdf");
    MEM32(VA_CAMPAIGN_FMT) = g_fmt_saved;
    MEM8(VA_INTRO_SEEN) = g_intro_seen;
    g_custom = 0;
    logf_("custom mode off: campaign profile restored");
}

/* The retail mission picker (entries +0xAC..+0xB0, 0x1C each; selected index +0x154 -- see
 * recomp_manual.c ma2_probe_missions, which calls this on every picker update): in Custom
 * Scenarios mode the editor's Test file starts selected, once per picker. */
void ma2_scn_mission_picker(uint32_t object)
{
    static uint32_t done;
    if (!g_custom || g_focus_mission < 0 || !object || object == done || object > 0x03FFFF00u) return;
    uint32_t first = MEM32(object + 0xAC), end = MEM32(object + 0xB0);
    if (!first || end < first || (end - first) % 0x1C) return;
    uint32_t count = (end - first) / 0x1C;
    if ((uint32_t)g_focus_mission >= count) return;
    MEM32(object + 0x154) = (uint32_t)g_focus_mission;
    done = object;
    logf_("mission picker: test scenario selected (entry %d of %u)", g_focus_mission, count);
}

/* Every menu frame (bots/ma2_bots_ui.c ma2_menu_policy_pulse calls this first). On the Campaign
 * Mission Picker (vtable 0x0048F3E0) in Custom Scenarios mode, the editor's Test file starts
 * selected: its forms (object +4..+8) hold a GUI barrel (vtable 0x00485778) with one item per
 * mission; sub_00208BF9 selects by value (eax=barrel, stack value, notify; callee pops 8). */
#define MISSION_PICKER_VT 0x0048F3E0u
#define GUI_BARREL_VT     0x00485778u
static int valid_widget(uint32_t w) { return w >= 0x10000 && w < 0x03FFF000; }
static uint32_t find_barrel(uint32_t w, int depth, int *budget)
{
    for (; valid_widget(w) && --*budget > 0; w = MEM32(w + 0x18)) {
        if (MEM32(w) == GUI_BARREL_VT) {
            uint32_t first = MEM32(w + 0xAC), end = MEM32(w + 0xB0);
            if (first && end > first && (end - first) % 0x1C == 0 && (end - first) / 0x1C == (uint32_t)g_nmission) return w;
        }
        if (depth < 12) { uint32_t r = find_barrel(MEM32(w + 0x1C), depth + 1, budget); if (r) return r; }
    }
    return 0;
}
#define HUB_MENU_VT   0x00481B70u
#define MULTI_MENU_VT 0x0048C228u
#define HUB_START_VT  0x00482170u   /* "Press Enter or click to start" */
#define NEWGAME_VT    0x0048C668u
#define LOBBY_VT      0x00482240u   /* Briefing Room */
#define INGAME_VT     0x00481EF8u
#define CAROUSEL_VT   0x004874A8u   /* hub / Multiplayer items */
#define DIFFICULTY_VT 0x0048BAC8u   /* campaign "Select Difficulty" */
#define AUTO_GIVE_UP_MS 15000u

/* Guest calls that may run game code with floating point (change notifications): keep it all. */
typedef struct FullRegs { Regs r; RecompXmm x[8]; double fp[8]; int top; uint16_t fcw, fcc; int fcmp, df; } FullRegs;
static void full_save(FullRegs *f)
{
    f->r = regs_save();
    f->x[0] = g_xmm0; f->x[1] = g_xmm1; f->x[2] = g_xmm2; f->x[3] = g_xmm3;
    f->x[4] = g_xmm4; f->x[5] = g_xmm5; f->x[6] = g_xmm6; f->x[7] = g_xmm7;
    memcpy(f->fp, g_fp_stack, sizeof f->fp); f->top = g_fp_top;
    f->fcw = g_fp_control_word; f->fcc = g_fp_cc; f->fcmp = g_fp_cmp; f->df = g_df;
}
static void full_restore(const FullRegs *f)
{
    regs_restore(&f->r);
    g_xmm0 = f->x[0]; g_xmm1 = f->x[1]; g_xmm2 = f->x[2]; g_xmm3 = f->x[3];
    g_xmm4 = f->x[4]; g_xmm5 = f->x[5]; g_xmm6 = f->x[6]; g_xmm7 = f->x[7];
    memcpy(g_fp_stack, f->fp, sizeof f->fp); g_fp_top = f->top;
    g_fp_control_word = f->fcw; g_fp_cc = f->fcc; g_fp_cmp = f->fcmp; g_df = f->df;
}
/* sub_00208BF9: select a barrel item by value (eax=barrel, value, notify; callee pops 8). */
static uint32_t barrel_pick(uint32_t barrel, uint32_t value, int notify)
{
    FullRegs f; full_save(&f);
    eax = barrel;
    PUSH32(esp, (uint32_t)notify); PUSH32(esp, value);
    PUSH32(esp, 0x002265D0u); RECOMP_ABI_CALL(0x00208BF9u, sub_00208BF9);
    uint32_t ok = eax & 0xFF;
    full_restore(&f);
    return ok;
}
static uint32_t barrel_items(uint32_t b) { uint32_t f = MEM32(b + 0xAC), e = MEM32(b + 0xB0); return f && e >= f && (e - f) % 0x1C == 0 ? (e - f) / 0x1C : 0; }
static int barrel_selected(uint32_t b)   /* value of the selected item, -1 if none */
{
    int32_t idx = (int32_t)MEM32(b + 0x154);
    if (idx < 0 || (uint32_t)idx >= barrel_items(b)) return -1;
    return (int32_t)MEM32(MEM32(b + 0xAC) + (uint32_t)idx * 0x1C + 0xC);
}
/* Item i's text (wide, begin/end at item +0/+4) equals `name`, ignoring case. */
static int item_named(uint32_t b, uint32_t i, const wchar_t *name)
{
    uint32_t item = MEM32(b + 0xAC) + i * 0x1C, p = MEM32(item), end = MEM32(item + 4);
    if (!valid_widget(p) || end < p || end - p > 512) return 0;
    size_t n = 0;
    for (; p + 2 <= end; p += 2, n++) {
        wchar_t c = (wchar_t)MEM16(p);
        if (!c) break;
        if (!name[n] || towupper(c) != towupper(name[n])) return 0;
    }
    return name[n] == 0;
}

extern void *xbox_FramebufferWindowHandle(void);
void pc_input_menu_click(void);   /* platform/pc_frontend.c: the menu's select button, as a mouse click gives it */
static int g_drop_press;   /* RECOMP_SCENARIO_AUTOSTART_DROP=1 (test): each screen's first press is lost */
static void post_key(unsigned vk)
{
    if (g_drop_press) { g_drop_press = 0; return; }
    HWND h = (HWND)xbox_FramebufferWindowHandle();
    if (!h) return;
    PostMessageA(h, WM_KEYDOWN, vk, 1);
    PostMessageA(h, WM_KEYUP, vk, (LPARAM)3 << 30);
}
/* The form of `menu` that is up (state bits 0-1 set), or 0 while it is still opening. */
static uint32_t ready_form(uint32_t menu)
{
    uint32_t begin = MEM32(menu + 4), end = MEM32(menu + 8);
    if (end < begin || end - begin > 1024) return 0;
    for (uint32_t e = begin; e < end; e += 4) {
        uint32_t form = MEM32(e);
        if (valid_widget(form) && (MEM32(form + 0x4C) & 3) == 3) return form;
    }
    return 0;
}
uint32_t ma2_gui_find(uint32_t form, uint32_t action);   /* pc_gui_bridge.c */
/* A hub / Multiplayer item, chosen the way a mouse click chooses it (pc_native_settings.cpp):
 * focus it (sub_001CAD40, edi=menu, esi=item) and press select. */
static int choose_item(uint32_t menu, uint32_t form, uint32_t action)
{
    uint32_t w = ma2_gui_find(form, action);
    if (!valid_widget(w) || MEM32(w) != CAROUSEL_VT || MEM32(w + 0x48) == 5) return 0;
    FullRegs f; full_save(&f);
    edi = menu; esi = w; ebx = 0;
    PUSH32(esp, 0x001CAEA0u); RECOMP_ABI_CALL(0x001CAD40u, sub_001CAD40);
    full_restore(&f);
    if (g_drop_press) g_drop_press = 0; else pc_input_menu_click();
    return 1;
}

static void auto_give_up(const char *why)
{
    auto_log("gave up %s; the menus are yours", why);
    editor_note(L"The test could not start by itself. Pick the scenario from the menu.");
    g_auto.state = AUTO_OFF;
    ma2_bots_test_release(1);
}

/* A screen can be up before it takes input (Press Start runs its prompt animation first), so a
 * press that changed nothing is repeated now and then while the same screen stays up. */
#define AUTO_RETRY_MS      2500u
#define AUTO_RETRY_WAIT_MS 6000u   /* after the last press, while loading */
#define AUTO_RETRIES       4
static int may_press(uint32_t object, DWORD every)
{
    if (object != g_auto.acted) {
        static int drop = -1;
        if (drop < 0) { const char *v = getenv("RECOMP_SCENARIO_AUTOSTART_DROP"); drop = v && *v && strcmp(v, "0"); }
        g_drop_press = drop;
        return 1;
    }
    return g_auto.tries < AUTO_RETRIES && GetTickCount() - g_auto.acted_at > every;
}
static void pressed(uint32_t object, const char *what)
{
    int again = object == g_auto.acted;
    g_auto.tries = again ? g_auto.tries + 1 : 0;
    g_auto.acted = object; g_auto.acted_at = GetTickCount(); g_auto.acted_frames = 0;
    if (g_auto.state == AUTO_WAIT_MATCH) g_auto.since = g_auto.acted_at;
    auto_log("%s%s", what, again ? " (again: the screen had not moved on)" : "");
}

/* New Game: the mode, then the map (by name: custom maps sit at a different place in each mode's
 * list), then select. menu+0x74 is the Game Type barrel (action 100), +0x70 the map (200). */
static void auto_newgame(uint32_t menu)
{
    uint32_t mode = MEM32(menu + 0x74), map = MEM32(menu + 0x70);
    if (!valid_widget(mode) || !valid_widget(map) || MEM32(mode) != GUI_BARREL_VT || MEM32(map) != GUI_BARREL_VT) return;
    const Scn *s = g_scn[g_focus];
    switch (g_auto.ng_phase) {
    case 0: {
        int want = g_auto.gametype, have = barrel_selected(mode);
        if (have == want || (want == 0xA && have == 1)) { g_auto.ng_phase = 1; g_auto.ng_wait = 0; return; }
        uint32_t n = barrel_items(mode);
        for (uint32_t i = 0; i < n; i++) {
            int v = (int32_t)MEM32(MEM32(mode + 0xAC) + i * 0x1C + 0xC);
            if (v == want || (want == 0xA && v == 1)) {
                if (g_auto.ng_wait++ % 30) return;   /* checked each frame, picked again now and then */
                uint32_t ok = barrel_pick(mode, (uint32_t)v, 1);
                auto_log("New Game: game type %s (value %d) -> %u", g_auto.mode, v, ok);
                return;
            }
        }
        {
            char list[160] = ""; size_t len = 0;
            for (uint32_t i = 0; i < n && len < sizeof list - 8; i++)
                len += (size_t)snprintf(list + len, sizeof list - len, "%s%d", i ? "," : "", (int32_t)MEM32(MEM32(mode + 0xAC) + i * 0x1C + 0xC));
            auto_log("New Game offers game types %s", list);
        }
        auto_give_up("at New Game: the game type is not offered");
        return;
    }
    case 1: {   /* the map list follows the mode; give it a few frames to be rebuilt */
        uint32_t n = barrel_items(map);
        for (uint32_t i = 0; i < n; i++) {
            if (!item_named(map, i, s->shown)) continue;
            uint32_t value = MEM32(MEM32(map + 0xAC) + i * 0x1C + 0xC);
            if ((uint32_t)barrel_selected(map) != value) {
                uint32_t ok = barrel_pick(map, value, 1);
                auto_log("New Game: map '%ls' (entry %u of %u, value %u) -> %u", s->shown, i, n, value, ok);
            }
            if ((uint32_t)barrel_selected(map) == value) { g_auto.ng_phase = 2; g_auto.ng_wait = 0; g_auto.since = GetTickCount(); }
            return;
        }
        if (++g_auto.ng_wait == 120) {
            auto_log("New Game: '%ls' is not in the %s map list (%u maps)", s->shown, g_auto.mode, n);
            auto_give_up("at New Game: the map is not listed for that mode");
        }
        return;
    }
    case 2:   /* let the screen (bot rows, side placement) catch up with the choices */
        if (++g_auto.ng_wait < 20) return;
        if (barrel_selected(map) < 0 || !item_named(map, MEM32(map + 0x154), s->shown)) { g_auto.ng_phase = 0; g_auto.ng_wait = 0; return; }
        post_key(VK_RETURN);
        g_auto.ng_phase = 3;
        pressed(menu, "New Game: start");
        return;
    }
}

/* One step per menu frame while a Test launch is being walked in. */
static void auto_frame(uint32_t object, uint32_t vtable)
{
    if (g_auto.state == AUTO_OFF) return;
    if (vtable == INGAME_VT) {
        auto_log("in the %s (%u ms after the last step)", g_scn[g_focus]->type ? "mission" : "match", GetTickCount() - g_auto.since);
        g_auto.state = AUTO_OFF;
        ma2_bots_test_release(0);
        return;
    }
    if (g_auto.state == AUTO_WAIT_MATCH) {
        /* A campaign with no difficulty chosen yet asks for one first: keep the highlighted one. */
        if (vtable == DIFFICULTY_VT && object != g_auto.acted) {
            if (ready_form(object) && ++g_auto.acted_frames >= 10) { post_key(VK_RETURN); pressed(object, "Select Difficulty: Enter"); }
            return;
        }
        /* Loading: nothing left to press -- unless the screen of the last press is still up a
         * while later (the press came before it took input): press again. */
        if (object != g_auto.acted) return;
        /* the menu still runs at full rate (a level load holds the menu loop up), long after */
        if (++g_auto.acted_frames < 300 || !may_press(object, AUTO_RETRY_WAIT_MS)) return;
        if (vtable == HUB_MENU_VT) {   /* Custom Scenarios not entered */
            uint32_t form = ready_form(object);
            if (g_custom || !form || !choose_item(object, form, 8)) return;
        } else if (vtable == LOBBY_VT || vtable == MISSION_PICKER_VT || vtable == DIFFICULTY_VT) post_key(VK_RETURN);
        else return;
        pressed(object, "the last step again");
        return;
    }
    if (object != g_auto.object) {
        g_auto.object = object; g_auto.vt = vtable; g_auto.frames = 0; g_auto.ng_phase = 0; g_auto.ng_wait = 0;
        g_auto.since = GetTickCount();
    }
    g_auto.frames++;
    if (GetTickCount() - g_auto.since > AUTO_GIVE_UP_MS) {
        char why[80]; snprintf(why, sizeof why, "on menu %08X: no next screen within %u s", vtable, AUTO_GIVE_UP_MS / 1000);
        auto_give_up(why);
        return;
    }
    uint32_t form = ready_form(object);
    if (!form || g_auto.frames < 3) return;
    {   /* RECOMP_SCENARIO_AUTOSTART_STALL=<menu vtable hex>: do nothing there (tests the give-up) */
        static long stall = -1;
        if (stall < 0) { const char *v = getenv("RECOMP_SCENARIO_AUTOSTART_STALL"); stall = v ? (long)strtoul(v, NULL, 16) : 0; }
        if (stall && (uint32_t)stall == vtable) return;
    }
    const int mp = g_scn[g_focus]->type == 0;
    switch (vtable) {
    case HUB_START_VT:
        if (!may_press(object, AUTO_RETRY_MS)) return;
        post_key(VK_RETURN); pressed(object, "Press Start: Enter");
        return;
    case HUB_MENU_VT:
        /* --browse (the game restarting after an edit in the in-game editor): Custom Scenarios,
         * or the hub itself when there is no picker to show (a multiplayer map, one mission) */
        if (g_auto.browse && mp) { auto_log("browse: stopped at the hub"); g_auto.state = AUTO_OFF; return; }
        if (!may_press(object, AUTO_RETRY_MS) || !choose_item(object, form, mp ? 4 : 8)) return;
        pressed(object, mp ? "hub: Multiplayer" : "hub: Custom Scenarios");
        return;
    case MULTI_MENU_VT:
        if (!mp || !may_press(object, AUTO_RETRY_MS) || !choose_item(object, form, 1)) return;
        pressed(object, "Multiplayer: Local");
        return;
    case NEWGAME_VT:
        if (!mp) return;
        if (g_auto.ng_phase < 3) auto_newgame(object);
        else if (may_press(object, AUTO_RETRY_MS)) { post_key(VK_RETURN); pressed(object, "New Game: start"); }
        return;
    case LOBBY_VT:
        /* after the lobby's side placement (bots/ma2_bots_ui.c place_hold, 30 frames) */
        if (!mp || g_auto.frames < 45 || !may_press(object, AUTO_RETRY_MS)) return;
        post_key(VK_RETURN); pressed(object, "Briefing Room: Enter");
        g_auto.state = AUTO_WAIT_MATCH;
        return;
    case MISSION_PICKER_VT:
        /* ma2_scn_menu_frame below selects the Test file first */
        if (mp || g_auto.frames < 10 || (uint32_t)g_focus_mission >= (uint32_t)g_nmission || !may_press(object, AUTO_RETRY_MS)) return;
        if (g_auto.browse) { auto_log("browse: stopped at Custom Scenarios"); g_auto.state = AUTO_OFF; return; }
        post_key(VK_RETURN); pressed(object, "mission picker: Enter");
        g_auto.state = AUTO_WAIT_MATCH;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* --quick-test (editor Preferences): straight in, straight out        */
/* ------------------------------------------------------------------ */
/* "MechAssault 2.exe --scenario f --quick-test ..." (or RECOMP_QUICK_TEST=1):
 *   - every movie until the first match/mission frame is skipped (the startup movies; a custom
 *     mission no longer gets the campaign intro at all, see Custom Scenarios mode) through the
 *     player's own skip (pc_input_movie_skip, honoured at sub_001F40B3 like a key press); movies
 *     inside the mission still play;
 *   - the Test autostart above walks the title and menus as usual;
 *   - once the match or mission has been up, reaching any front-end menu (results passed, Quit to
 *     Main Hub, mission complete/failed) closes the game the way Exit Game does (WM_CLOSE). After
 *     pause -> Quit the results screen is confirmed for the player: they already chose to leave.
 * The window's close button exits as always. Without the flag nothing here runs. */
#define PAUSE_VT        0x004827B0u
#define QUIT_CONFIRM_VT 0x0048BFE8u   /* pause -> Quit (p_quit.ggf) */
#define RESULTS_VT      0x00482638u   /* end-of-match results, also after pause -> Quit */
#define CREDITS_VT      0x0048BD88u
int pc_display_movie_active(void);   /* platform/pc_frontend.c */
void pc_input_movie_skip(void);
static int g_quick;
static volatile LONG g_quick_in_game;
/* --skip-intro (the game restarting itself for the in-game editor): the startup movies are skipped
 * like --quick-test's until the Test autostart has walked in (or stopped at Custom Scenarios). */
static int g_skip_intro;
static volatile LONG g_intro_done;

static DWORD WINAPI quick_movies(LPVOID unused)
{
    (void)unused;
    int was = 0; DWORD since = 0, pressed = 0;
    while (!g_quick_in_game && !g_intro_done) {
        int on = pc_display_movie_active();
        if (on) {
            if (!was) { logf_("quick test: movie skipped"); since = GetTickCount(); }
            pc_input_movie_skip();
            /* the Attract Screen's movies ignore that request: press Enter as a player would */
            if (GetTickCount() - since > 1500 && GetTickCount() - pressed > 700) {
                HWND h = (HWND)xbox_FramebufferWindowHandle();
                if (h && pc_display_movie_active()) {
                    PostMessageA(h, WM_KEYDOWN, VK_RETURN, 1); Sleep(60);
                    PostMessageA(h, WM_KEYUP, VK_RETURN, (LPARAM)3 << 30);
                    if (!pressed) logf_("quick test: movie still playing: Enter");
                }
                pressed = GetTickCount();
            }
        } else pressed = 0;
        was = on;
        Sleep(15);
    }
    return 0;
}

/* WinMain (main.c), before the guest starts. */
void ma2_scn_startup(void)
{
    const char *env = getenv("RECOMP_QUICK_TEST");
    if (env && *env) g_quick = strcmp(env, "0") != 0;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (!_wcsicmp(argv[i], L"--quick-test")) g_quick = 1;
        if (!_wcsicmp(argv[i], L"--skip-intro")) g_skip_intro = 1;
    }
    if (argv) LocalFree(argv);
    if (!g_quick && !g_skip_intro) return;
    if (g_quick) logf_("quick test: movies skipped, the game closes when the match or mission is left");
    else logf_("skip intro: startup movies skipped (restarted from the in-game editor)");
    HANDLE t = CreateThread(NULL, 0, quick_movies, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* A mission that is lost or won while it loads (dogfood D11: an empty 'Destroyed' trigger -> Lose)
 * never shows a match frame: the game drops the level and the custom campaign starts it again from
 * the hub, for ever. Its unload counts as having been in it, so the next front-end menu closes the
 * game as usual; should the game restart the level without any menu, a second unload of a scenario
 * level with no match frame in between closes it too. */
void ma2_pc_sticky_keys(int suppress);
static volatile LONG g_quick_level_scn, g_quick_level_frames, g_quick_bare_unloads;

static void quick_close(const char *why)
{
    logf_("quick test: %s: closing the game", why);
    HWND h = (HWND)xbox_FramebufferWindowHandle();
    if (h) PostMessageA(h, WM_CLOSE, 0, 0);
    else { ma2_pc_sticky_keys(0); ExitProcess(0); }
}

static void quick_level_loaded(int scenario)
{
    InterlockedExchange(&g_quick_level_scn, scenario);
    InterlockedExchange(&g_quick_level_frames, 0);
}

static void quick_level_unloaded(void)
{
    if (!g_quick || !InterlockedExchange(&g_quick_level_scn, 0)) return;
    if (g_quick_level_frames) { InterlockedExchange(&g_quick_bare_unloads, 0); InterlockedExchange(&g_quick_in_game, 1); return; }
    LONG n = InterlockedIncrement(&g_quick_bare_unloads);
    logf_("quick test: the scenario level was dropped before its first match frame (%ld)", n);
    InterlockedExchange(&g_quick_in_game, 1);
    if (n >= 2) quick_close("the mission ends as soon as it starts (lost or won while loading)");
}

static int front_end(uint32_t vt)
{
    return vt == HUB_START_VT || vt == HUB_MENU_VT || vt == MULTI_MENU_VT || vt == NEWGAME_VT || vt == LOBBY_VT ||
           vt == MISSION_PICKER_VT || vt == CREDITS_VT;
}

static void quick_frame(uint32_t object, uint32_t vtable)
{
    static uint32_t last_vt, seen, unknown;
    static int quit_chosen, closing, presses;
    static DWORD seen_at, pressed_at;
    if (!g_quick || closing) return;
    uint32_t prev = last_vt; last_vt = vtable;
    if (vtable == INGAME_VT) {
        if (!g_quick_in_game) logf_("quick test: in the match/mission");
        InterlockedExchange(&g_quick_in_game, 1);
        InterlockedExchange(&g_quick_level_frames, 1);
        quit_chosen = 0;
        return;
    }
    if (!g_quick_in_game || vtable == PAUSE_VT || vtable == QUIT_CONFIRM_VT) { quit_chosen = 0; return; }
    if (vtable == RESULTS_VT) {
        if (prev == QUIT_CONFIRM_VT) { quit_chosen = 1; presses = 0; }
        if (quit_chosen && ready_form(object) && presses < 6 && GetTickCount() - pressed_at > 1500) {
            post_key(VK_RETURN); presses++; pressed_at = GetTickCount();
            logf_("quick test: results after Quit: Enter");
        }
        return;
    }
    if (!front_end(vtable)) {
        if (vtable != unknown) { unknown = vtable; logf_("quick test: menu %08X (not a front-end menu; staying)", vtable); }
        return;
    }
    /* the menu is up and the game has finished its way back (profile writes on the way). A custom
     * mission passes the hub only for a moment (the custom campaign starts the mission again from
     * it), so there the first hub frame is the way out; its saves go to MAScns.sav anyway. */
    if (object != seen) { seen = object; seen_at = GetTickCount(); }
    if (!(g_custom && vtable == HUB_MENU_VT) && (!ready_form(object) || GetTickCount() - seen_at < 500)) return;
    closing = 1;
    char why[64];
    snprintf(why, sizeof why, "left the match/mission (menu %08X)", vtable);
    quick_close(why);
}

/* ------------------------------------------------------------------ */
/* The Scenario Editor, opened from Custom Scenarios                    */
/* ------------------------------------------------------------------ */
/* The Editor button left of the scenario (pc_native_settings.cpp draws it; click, E or pad Y)
 * asks for the editor here (ma2_scn_editor_open). On the next mission picker frame the game
 * starts "MA2 Scenario Editor.exe" from its own folder on the selected scenario:
 *     --from-game <game window> --from-game-result <file> [--file <scenario>] [--automation <pipe>]
 * waits for it (the process handle, once per menu frame), hides its own window once the editor's
 * is up and mutes its sound (ma2_scenario_audio.cpp), then comes back. The editor's exit code and
 * saved list (ux\spec-ingame-editor.md section 6) say what happened:
 *   2 + "test=<file>"  the editor's Test: the game restarts straight into that scenario;
 *   0 + "saved=<file>" lines: a scenario the game had loaded changed -> it restarts on Custom
 *                      Scenarios with it selected (--browse; a loaded scenario cannot be swapped
 *                      in place); only new files -> rescan (hub) and back into Custom Scenarios;
 *   anything else      the editor crashed: a note, then as 0 with whatever it saved.
 * A hidden game (RECOMP_FB_HIDDEN, tests) starts the editor under --automation (hidden too); the
 * pipe is RECOMP_EDITOR_PIPE, default "ma2se-ingame". */
static volatile LONG g_editor_request;
static HANDLE g_editor_proc;
static DWORD g_editor_pid, g_editor_started;
static int g_editor_hid_game;          /* the game window was hidden for the editor */
static int g_editor_reenter;           /* back to Custom Scenarios through the hub (new scenarios) */
static int g_editor_from_hub;          /* started from the no-missions prompt on the hub, not the picker */
static uint32_t g_empty_dialog;        /* that prompt's dialog (pc_native_settings.cpp ma2_gui_dialog) */
uint32_t ma2_gui_dialog(uint32_t title, uint32_t message);   /* pc_settings.h */
uint32_t ma2_gui_dialog_result(uint32_t dialog);
void ma2_gui_text(uint32_t widget, uint32_t text);           /* pc_gui_bridge.c */
static wchar_t g_editor_result[MAX_PATH], g_editor_select[MAX_PATH];
static wchar_t g_editor_note[200];
static DWORD g_editor_note_at, g_editor_note_ms = 8000u;
void ma2_scn_audio_mute_process(int mute);   /* ma2_scenario_audio.cpp */
void ma2_pc_editor_focus_clear(void);        /* pc_native_settings.cpp: Editor button focus off */

static int editor_exe(wchar_t *out, size_t cap)
{
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    wchar_t *slash = wcsrchr(self, L'\\');
    if (!slash) return 0;
    *slash = 0;
    swprintf(out, cap, L"%ls\\MA2 Scenario Editor.exe", self);
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}
static int game_hidden(void)
{
    const char *v = getenv("RECOMP_FB_HIDDEN");
    return v && *v && strcmp(v, "0");
}
int ma2_scn_editor_available(void)
{
    static int have = -1;
    if (have < 0) { wchar_t exe[MAX_PATH]; have = editor_exe(exe, MAX_PATH); }
    return have;
}
int ma2_scn_editor_running(void) { return g_editor_proc != NULL; }
void ma2_scn_editor_open(void) { if (!g_editor_proc) InterlockedExchange(&g_editor_request, 1); }
/* A note for pc_native_settings.cpp to show over the menu for a few seconds (NULL: none). */
const wchar_t *ma2_scn_editor_note(void)
{
    return g_editor_note[0] && GetTickCount() - g_editor_note_at < g_editor_note_ms ? g_editor_note : NULL;
}
static void editor_note_for(const wchar_t *text, DWORD ms)
{
    wcsncpy(g_editor_note, text, 199); g_editor_note[199] = 0;
    g_editor_note_at = GetTickCount();
    g_editor_note_ms = ms;
    logf_("editor: note '%ls'", text);
}
static void editor_note(const wchar_t *text) { editor_note_for(text, 8000u); }
/* W-14a: one note for the scenario files skipped since the last one. */
static void skip_notes(void)
{
    int fresh = 0, first = -1;
    for (int i = 0; i < g_nskipped; i++) if (!g_skipped[i].noted) { fresh++; if (first < 0) first = i; g_skipped[i].noted = 1; }
    if (!fresh) return;
    const wchar_t *b = wcsrchr(g_skipped[first].file, L'\\'); b = b ? b + 1 : g_skipped[first].file;
    wchar_t text[200];
    if (fresh == 1) swprintf(text, 200, L"Scenario file not listed: %.60ls %ls", b, g_skipped[first].why);
    else swprintf(text, 200, L"%d scenario files not listed. %.60ls %ls", fresh, b, g_skipped[first].why);
    editor_note_for(text, 12000u);
}

typedef struct { DWORD pid; int found; } WinFind;
static BOOL CALLBACK find_visible(HWND h, LPARAM p)
{
    WinFind *f = (WinFind *)p; DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == f->pid && IsWindowVisible(h)) { f->found = 1; return FALSE; }
    return TRUE;
}

/* The scenario selected in the mission picker `object`, or NULL. */
static const Scn *picker_scenario(uint32_t object)
{
    uint32_t begin = MEM32(object + 4), end = MEM32(object + 8);
    if (end < begin || end - begin > 1024) return NULL;
    for (uint32_t e = begin; e < end; e += 4) {
        int budget = 2000;
        uint32_t barrel = valid_widget(MEM32(e)) ? find_barrel(MEM32(MEM32(e) + 0x38), 0, &budget) : 0;
        if (!barrel) continue;
        int v = barrel_selected(barrel);
        return v >= 0 && v < g_nmission ? g_mission_scn[v] : NULL;
    }
    return NULL;
}

static void editor_start(uint32_t object)
{
    wchar_t exe[MAX_PATH];
    if (!editor_exe(exe, MAX_PATH)) { editor_note(L"The Scenario Editor is not installed next to the game."); return; }
    const Scn *s = object ? picker_scenario(object) : NULL;   /* 0: from the hub, the editor's start page */
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) tmp[0] = 0;
    swprintf(g_editor_result, MAX_PATH, L"%lsMA2SE-from-game-%lu.txt", tmp, GetCurrentProcessId());
    DeleteFileW(g_editor_result);
    HWND game = (HWND)xbox_FramebufferWindowHandle();
    static wchar_t cmd[4 * MAX_PATH];
    const size_t cap = sizeof cmd / sizeof *cmd;
    int len = swprintf(cmd, cap, L"\"%ls\" --from-game %llu --from-game-result \"%ls\"", exe,
                       (unsigned long long)(uintptr_t)game, g_editor_result);
    if (s && len > 0) len += swprintf(cmd + len, cap - (size_t)len, L" --file \"%ls\"", s->file);
    if (game_hidden() && len > 0) {
        char pipe[64]; const char *v = getenv("RECOMP_EDITOR_PIPE");
        snprintf(pipe, sizeof pipe, "%s", v && *v ? v : "ma2se-ingame");
        len += swprintf(cmd + len, cap - (size_t)len, L" --automation %hs", pipe);
    }
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si; memset(&pi, 0, sizeof pi);
    wchar_t dir[MAX_PATH]; wcsncpy(dir, exe, MAX_PATH - 1); dir[MAX_PATH - 1] = 0;
    wchar_t *slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        logf_("editor: could not start %ls (error %lu)", exe, GetLastError());
        editor_note(L"The Scenario Editor could not be started.");
        return;
    }
    CloseHandle(pi.hThread);
    g_editor_proc = pi.hProcess; g_editor_pid = pi.dwProcessId; g_editor_started = GetTickCount();
    g_editor_hid_game = 0;
    if (!game_hidden()) AllowSetForegroundWindow(pi.dwProcessId);
    ma2_scn_audio_mute_process(1);
    { static char shown[4 * MAX_PATH * 2]; fprintf(stderr, "[SCN] editor pid %lu: %s\n", pi.dwProcessId, xbox_log_pathw(cmd, shown, sizeof shown)); fflush(stderr); }
    logf_("editor: started pid %lu on %ls", pi.dwProcessId, s ? s->file : L"(start page)");
}

/* Start this game again on `file` (the editor's Test, or a changed scenario: --browse) and end
 * this copy: a scenario the game has loaded cannot be swapped in place. */
void ma2_single_instance_release(void);   /* main.c */
static void restart_game(const wchar_t *file, int browse)
{
    wchar_t self[MAX_PATH], dir[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wcsncpy(dir, self, MAX_PATH - 1); dir[MAX_PATH - 1] = 0;
    wchar_t *slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0;
    static wchar_t cmd[3 * MAX_PATH];
    swprintf(cmd, sizeof cmd / sizeof *cmd, L"\"%ls\" --scenario \"%ls\" --skip-intro%ls", self, file, browse ? L" --browse" : L"");
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si; memset(&pi, 0, sizeof pi);
    /* this copy ends right after: free the single-instance lock first, or the new copy says
     * "MechAssault 2 is already running" (main.c also waits a few seconds for it) */
    ma2_single_instance_release();
    if (!CreateProcessW(self, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        logf_("editor: could not restart the game (error %lu)", GetLastError());
        editor_note(L"Restart the game to load your changes.");
        return;
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    { static char shown[3 * MAX_PATH * 2]; fprintf(stderr, "[SCN] restarting as pid %lu: %s\n", pi.dwProcessId, xbox_log_pathw(cmd, shown, sizeof shown)); fflush(stderr); }
    logf_("editor: restarting the game (%ls)", browse ? L"Custom Scenarios" : L"play");
    /* nothing of a custom session is written on the way out (the profile is the player's own) */
    ma2_pc_sticky_keys(0); /* W-02: TerminateProcess runs no restore path */
    TerminateProcess(GetCurrentProcess(), 0);
}

static void editor_back(void)
{
    DWORD code = 0;
    GetExitCodeProcess(g_editor_proc, &code);
    CloseHandle(g_editor_proc); g_editor_proc = NULL;
    ma2_pc_editor_focus_clear();   /* before any key is posted below (Esc to the hub) */
    ma2_scn_audio_mute_process(0);
    HWND game = (HWND)xbox_FramebufferWindowHandle();
    if (g_editor_hid_game && game) { ShowWindowAsync(game, SW_SHOW); SetForegroundWindow(game); }
    g_editor_hid_game = 0;
    fprintf(stderr, "[SCN] editor returned code %lu after %lu s\n", code, (GetTickCount() - g_editor_started) / 1000); fflush(stderr);
    /* the saved list: opened=<file>, saved=<file> lines, test=<file> */
    wchar_t play[MAX_PATH] = L"", changed[MAX_PATH] = L"", added[MAX_PATH] = L"";
    uint32_t size = 0;
    uint8_t *text = read_file_w(g_editor_result, &size);
    if (text) {
        char *p = (char *)text, *end = p + size;
        while (p < end) {
            char *nl = memchr(p, '\n', (size_t)(end - p)); if (!nl) nl = end;
            char *line_end = nl; if (line_end > p && line_end[-1] == '\r') line_end--;
            int is_play = line_end - p > 5 && (!memcmp(p, "play=", 5) || !memcmp(p, "test=", 5)), is_saved = line_end - p > 6 && !memcmp(p, "saved=", 6);
            if (is_play || is_saved) {
                const char *v = p + (is_play ? 5 : 6);
                wchar_t path[MAX_PATH];
                int n = MultiByteToWideChar(CP_UTF8, 0, v, (int)(line_end - v), path, MAX_PATH - 1);
                path[n > 0 ? n : 0] = 0;
                if (is_play) wcscpy(play, path);
                else if (loaded_path(path)) wcscpy(changed, path);
                else wcscpy(added, path);
                logf_("editor: %s %ls", is_play ? "test" : "saved", path);
            }
            p = nl + 1;
        }
        free(text);
        DeleteFileW(g_editor_result);
    }
    if (code != 0 && code != 2) {
        wchar_t note[200];
        swprintf(note, 200, L"The editor closed unexpectedly (code 0x%08lX).\nIt offers your last autosave next time you open it.", code);
        editor_note(note);
    }
    if (code == 2 && play[0]) { restart_game(play, 0); return; }
    if (changed[0]) { restart_game(changed, 1); return; }
    if (added[0] && g_editor_from_hub) {   /* already on the hub: rescan here, then Custom Scenarios if it has missions now */
        g_editor_from_hub = 0;
        rescan();
        if (g_nmission) { wcscpy(g_editor_select, added); g_editor_reenter = 1; }
    } else if (added[0]) {   /* rescan on the hub, then Custom Scenarios again on the new file */
        wcscpy(g_editor_select, added);
        g_editor_reenter = 1;
        post_key(VK_ESCAPE);
    }
    g_editor_from_hub = 0;
}

/* Every menu frame, first. Returns 1 while the editor is up (nothing else runs then). */
static int editor_frame(uint32_t object, uint32_t vtable)
{
    if (g_editor_proc) {
        /* started from the hub: hold its idle timer (sub_001F34BE: 45 s since +0x88 -> Attract Screen,
         * where no menu frame runs to notice the editor closing) */
        if (vtable == HUB_MENU_VT && valid_widget(object)) MEM32(object + 0x88) = MEM32(0x004E2430u);
        if (WaitForSingleObject(g_editor_proc, 0) == WAIT_OBJECT_0) { editor_back(); return 0; }
        if (!g_editor_hid_game && !game_hidden()) {   /* hide the game once the editor's window is up */
            WinFind f = { g_editor_pid, 0 };
            EnumWindows(find_visible, (LPARAM)&f);
            HWND game = (HWND)xbox_FramebufferWindowHandle();
            if ((f.found || GetTickCount() - g_editor_started > 20000u) && game) { ShowWindowAsync(game, SW_HIDE); g_editor_hid_game = 1; }
        }
        Sleep(50);   /* a menu frame now and then: the game idles while the editor runs */
        return 1;
    }
    {   /* RECOMP_SCENARIO_OPEN_EDITOR=1 (tests, rc_smoke S10c): press the Editor button once on Custom Scenarios */
        static int auto_open = -1;
        if (auto_open < 0) { const char *v = getenv("RECOMP_SCENARIO_OPEN_EDITOR"); auto_open = v && *v && strcmp(v, "0"); }
        if (auto_open && vtable == MISSION_PICKER_VT && g_custom && g_auto.state == AUTO_OFF && ready_form(object)) { auto_open = 0; ma2_scn_editor_open(); }
    }
    if (g_empty_prompt && vtable == HUB_MENU_VT) {   /* Custom Scenarios with no missions (ma2_scn_custom_enter) */
        g_empty_prompt = 0;
        if (!ma2_scn_editor_available()) editor_note(L"No custom missions yet, and the Scenario Editor is not installed next to the game.");
        else if (!g_empty_dialog) {
            g_empty_dialog = ma2_gui_dialog(ma2_pc_wide(L"CUSTOM SCENARIOS"),
                ma2_pc_wide(L"No custom missions yet. Open the Scenario Editor to make one?"));
            /* ma2_gui_dialog labels its confirm button (action 0x466) "Exit game" for the exit prompt;
             * this dialog is the newest on the modal stack (pc_gui_bridge.c dialog_form(0)) */
            uint32_t begin = MEM32(0x5240D0u), end = MEM32(0x5240D4u);
            uint32_t form = end > begin && end - begin <= 4096 ? MEM32(MEM32(end - 4) + 4) : 0;
            uint32_t button = form ? ma2_gui_find(form, 0x466) : 0;
            if (button) ma2_gui_text(button, ma2_pc_wide(L"Open editor"));
        }
    }
    if (g_empty_dialog) {   /* 1 or 3 = confirmed, as the exit dialog (pc_native_settings.cpp) */
        uint32_t result = ma2_gui_dialog_result(g_empty_dialog);
        if (result) {
            g_empty_dialog = 0;
            logf_("custom mode: editor prompt result %u", result);
            if ((result == 1 || result == 3) && vtable == HUB_MENU_VT) { g_editor_from_hub = 1; editor_start(0); }
        }
    }
    if (InterlockedExchange(&g_editor_request, 0)) {
        if (vtable == MISSION_PICKER_VT && g_custom && valid_widget(object)) editor_start(object);
        else logf_("editor: asked for off the Custom Scenarios screen (menu %08X), ignored", vtable);
    }
    if (g_editor_reenter && vtable == HUB_MENU_VT) {
        /* ma2_scn_menu_frame ran rescan() on this hub visit; enter Custom Scenarios on the new file */
        uint32_t form = ready_form(object);
        if (form && ++g_editor_reenter > 10) {
            for (int i = 0; i < g_nscn; i++)
                if (!_wcsicmp(g_scn[i]->file, g_editor_select)) {
                    g_focus = i;
                    for (int m = 0; m < g_nmission; m++) if (g_mission_scn[m] == g_scn[i]) g_focus_mission = m;
                }
            if (choose_item(object, form, 8)) logf_("editor: back into Custom Scenarios on %ls (entry %d)", g_editor_select, g_focus_mission);
            g_editor_reenter = 0;
        }
    }
    return 0;
}

void ma2_scn_menu_frame(uint32_t object, uint32_t vtable)
{
    static uint32_t done, last_menu;
    if (editor_frame(object, vtable)) return;
    if (g_custom) MEM8(VA_INTRO_SEEN) = 1;   /* no campaign intro before a custom mission */
    auto_frame(object, vtable);
    if (g_skip_intro && !g_quick && g_auto.state == AUTO_OFF) InterlockedExchange(&g_intro_done, 1);
    quick_frame(object, vtable);
    if (vtable == HUB_MENU_VT || vtable == MULTI_MENU_VT) {
        /* once per visit of the hub / Multiplayer menu: pick up scenarios saved since boot */
        if (object != last_menu && g_ready) { last_menu = object; rescan(); skip_notes(); }
        return;
    }
    last_menu = 0;
    if (vtable != MISSION_PICKER_VT || !g_custom || g_focus_mission < 0 || !valid_widget(object) || object == done) return;
    uint32_t begin = MEM32(object + 4), end = MEM32(object + 8);
    if (end < begin || end - begin > 1024) return;
    for (uint32_t e = begin; e < end; e += 4) {
        int budget = 2000;
        uint32_t barrel = valid_widget(MEM32(e)) ? find_barrel(MEM32(MEM32(e) + 0x38), 0, &budget) : 0;
        if (!barrel) continue;
        Regs r = regs_save();
        eax = barrel;
        PUSH32(esp, 1); PUSH32(esp, (uint32_t)g_focus_mission);
        PUSH32(esp, 0x002265D0u); RECOMP_ABI_CALL(0x00208BF9u, sub_00208BF9);
        uint32_t ok = eax & 0xFF;
        regs_restore(&r);
        done = object;
        logf_("mission picker: test scenario selected (entry %d of %d) -> %u", g_focus_mission, g_nmission, ok);
        return;
    }
}

/* Top of the hub's activation check (recomp_0051.c sub_001F3BCC): Custom Scenarios (8) is entered
 * by the hook further down that function; every other hub item ends the mode first, so the
 * Campaign button always meets the player's own profile. The mode is NOT ended when the hub is
 * merely rebuilt: quitting a custom mission passes the hub on its way back to the mission picker. */
void ma2_scn_hub_action(uint32_t action)
{
    if (action != 8) ma2_scn_custom_leave();
}

static const wchar_t *ui_text(uint32_t id)
{
    switch (id - SCN_UI_BASE) {
    case 0: return L"Custom Scenarios";
    case 1: return g_nmission ? L"Play missions made with the MechAssault 2 Scenario Editor."
                              : L"No custom missions yet. Make one with the MechAssault 2 Scenario Editor.";
    default: return NULL;
    }
}

/* Text for the 0x00F00000..0x00FFFFFF range (pc_prompts.cpp ma2_pc_localized). */
uint32_t ma2_scn_text(uint32_t id, uint32_t original)
{
    if (id >= SCN_UI_BASE) { const wchar_t *w = ui_text(id); return w ? ma2_pc_wide(w) : original; }
    if (!g_ready) return original;
    if (id >= SCN_NAME_BASE) {
        uint32_t slot = (id - SCN_NAME_BASE) / 2;
        if (slot < (uint32_t)g_nscn) {
            const Scn *s = g_scn[slot];
            const wchar_t *w = (id & 1) ? s->desc_shown : s->shown;
            if (w[0]) return ma2_pc_wide(w);
        }
        return original;
    }
    if (id >= SCN_TEXT_GLOBAL) {   /* each scenario owns [text_base, text_base + nstrings) */
        uint32_t rel = id - SCN_TEXT_GLOBAL;
        for (int i = 0; i < g_nscn; i++) {
            const Scn *s = g_scn[i];
            if (rel >= s->text_base && rel < s->text_base + (uint32_t)s->nstrings) {
                const wchar_t *w = s->strings[rel - s->text_base].text;
                return w ? ma2_pc_wide(w) : original;
            }
        }
    }
    return original;
}

int ma2_scn_count(void) { return g_nscn; }

/* ------------------------------------------------------------------ */
/* imported audio clips                                                */
/* ------------------------------------------------------------------ */
/* Global cue 0x5C100000 + slot*0x1000 + n -> the WAV bytes of [Audio] n inside that .ma2map
 * (the archive stays in memory for the whole run). */
const uint8_t *ma2_scn_clip(uint32_t cue, uint32_t *size)
{
    if (cue <= SCN_CUE_GLOBAL || cue >= SCN_CUE_GLOBAL + SCN_MAX * 0x1000u) return NULL;
    uint32_t rel = cue - SCN_CUE_GLOBAL, slot = rel / 0x1000u, n = rel % 0x1000u;
    if (slot >= (uint32_t)g_nscn || n == 0 || n >= 64) return NULL;
    const Scn *s = g_scn[slot];
    if (!s->audio[n][0]) return NULL;
    const MgfEntry *e = mgf_find(&s->mgf, s->audio[n]);
    if (!e) { logf_("%s: [Audio] %u names missing %s", s->id, n, s->audio[n]); return NULL; }
    *size = e->size;
    return s->mgf.raw + e->offset;
}

int ma2_scn_audio_play(uint32_t cue);   /* ma2_scenario_audio.cpp */

/* Top of MAActionSound / MAActionPlayVoice Execute (recomp_0033.c / recomp_0042.c): the raw cue id
 * the action was authored with. Retail ids are left to the game. */
void ma2_scn_action_cue(uint32_t cue)
{
    static int trace = -1;
    if (trace < 0) { const char *t = getenv("RECOMP_SCENARIO_TRACE"); trace = t && *t && strcmp(t, "0"); }
    if (trace) logf_("action cue %08X", cue);
    if (cue > SCN_CUE_GLOBAL && cue < SCN_CUE_GLOBAL + SCN_MAX * 0x1000u) ma2_scn_audio_play(cue);
}
