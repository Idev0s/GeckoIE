/* omnislim: copies a zip/jar (omni.ja) without the entries whose names start with a listed prefix.
   No recompression: compressed data is copied as is, local headers and the central directory are rewritten.
   usage: omnislim in.ja out.ja prefixes.txt     exit 0 = wrote out.ja, 2 = nothing to remove, other = error */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static uint16_t r16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void w16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
typedef struct { uint32_t lho; uint32_t cpos; int keep; } Ent;
static int cmp(const void* a, const void* b) { uint32_t x = ((const Ent*)a)->lho, y = ((const Ent*)b)->lho; return x < y ? -1 : x > y; }
static int hasPrefix(const char* file, const char* prefix) {   /* 0 = at least one entry starts with the prefix */
  FILE* f = fopen(file, "rb"); uint8_t* in; long len, eo = -1; int total, i; uint32_t p; size_t pl = strlen(prefix);
  if (!f) return 2; fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET); in = (uint8_t*)malloc(len + 1);
  if (!in || fread(in, 1, len, f) != (size_t)len) { fclose(f); return 2; } fclose(f);
  for (long q = len - 22; q >= 0 && q > len - 22 - 65536; q--) if (r32(in + q) == 0x06054b50) { eo = q; break; }
  if (eo < 0) return 2; total = r16(in + eo + 10); p = r32(in + eo + 16);
  for (i = 0; i < total; i++) {
    int nl; if (p + 46 > (uint32_t)len || r32(in + p) != 0x02014b50) return 2; nl = r16(in + p + 28);
    if ((size_t)nl >= pl && !memcmp(in + p + 46, prefix, pl)) return 0;
    p += 46 + nl + r16(in + p + 30) + r16(in + p + 32);
  }
  return 1;
}
int main(int argc, char** argv) {
  FILE* f; uint8_t* in; long len; char pat[256][256]; int np = 0, i;
  if (argc >= 4 && !strcmp(argv[1], "--has")) return hasPrefix(argv[2], argv[3]);
  if (argc < 4) { fprintf(stderr, "usage: omnislim in.ja out.ja prefixes.txt\n"); return 1; }
  f = fopen(argv[3], "r"); if (!f) { fprintf(stderr, "cannot read %s\n", argv[3]); return 1; }
  while (np < 256 && fgets(pat[np], 256, f)) { char* e = pat[np] + strlen(pat[np]); while (e > pat[np] && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0; if (pat[np][0] && pat[np][0] != '#') np++; }
  fclose(f);
  f = fopen(argv[1], "rb"); if (!f) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
  fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET); in = (uint8_t*)malloc(len + 1);
  if (!in || fread(in, 1, len, f) != (size_t)len) { fprintf(stderr, "read failed\n"); return 1; } fclose(f);
  long eo = -1; for (long p = len - 22; p >= 0 && p > len - 22 - 65536; p--) if (r32(in + p) == 0x06054b50) { eo = p; break; }
  if (eo < 0) { fprintf(stderr, "not a zip file\n"); return 1; }
  int total = r16(in + eo + 10); uint32_t cdoff = r32(in + eo + 16); int clen = r16(in + eo + 20);
  Ent* en = (Ent*)calloc(total + 1, sizeof(Ent)); uint32_t p = cdoff; int removed = 0;
  for (i = 0; i < total; i++) {
    if (p + 46 > (uint32_t)len || r32(in + p) != 0x02014b50) { fprintf(stderr, "bad central directory\n"); return 1; }
    int nl = r16(in + p + 28), el = r16(in + p + 30), cl = r16(in + p + 32); const char* name = (const char*)in + p + 46; int k;
    en[i].lho = r32(in + p + 42); en[i].cpos = p; en[i].keep = 1;
    for (k = 0; k < np; k++) { size_t pl = strlen(pat[k]); if ((size_t)nl >= pl && !memcmp(name, pat[k], pl)) { en[i].keep = 0; break; } }
    if (!en[i].keep) removed++;
    p += 46 + nl + el + cl;
  }
  if (!removed) { printf("nothing to remove\n"); return 2; }
  qsort(en, total, sizeof(Ent), cmp);
  uint8_t* out = (uint8_t*)malloc(len + 65536); uint32_t o = 0; uint8_t* cd = (uint8_t*)malloc(len); uint32_t co = 0; int kept = 0;
  for (i = 0; i < total; i++) {
    if (!en[i].keep) continue;
    uint8_t* c = in + en[i].cpos; int nl = r16(c + 28), el = r16(c + 30), cl = r16(c + 32); uint16_t fl = r16(c + 8); uint32_t cs = r32(c + 20);
    uint8_t* l = in + en[i].lho; if (r32(l) != 0x04034b50) { fprintf(stderr, "bad local header\n"); return 1; }
    uint32_t span = 30 + r16(l + 26) + r16(l + 28) + cs;
    if (fl & 8) { uint8_t* d = l + span; span += (r32(d) == 0x08074b50) ? 16 : 12; }
    memcpy(out + o, l, span);
    memcpy(cd + co, c, 46 + nl + el + cl); w32(cd + co + 42, o); co += 46 + nl + el + cl; o += span; kept++;
  }
  memcpy(out + o, cd, co); uint32_t newcd = o; o += co;
  memcpy(out + o, in + eo, 22 + clen); w16(out + o + 8, (uint16_t)kept); w16(out + o + 10, (uint16_t)kept); w32(out + o + 12, co); w32(out + o + 16, newcd); o += 22 + clen;
  f = fopen(argv[2], "wb"); if (!f || fwrite(out, 1, o, f) != o) { fprintf(stderr, "write failed\n"); return 1; } fclose(f);
  printf("removed %d of %d entries; %ld -> %u bytes (saved %ld KB)\n", removed, total, len, o, (len - (long)o) / 1024);
  return 0;
}
