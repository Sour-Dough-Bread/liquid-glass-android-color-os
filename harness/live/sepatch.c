/* Test stand-in for a root manager's live policy tool (ksud sepolicy apply,
 * magiskpolicy --live --apply). Host side: reads a binary policy pulled from
 * /sys/fs/selinux/policy, adds allow rules, writes the result for the device
 * to load through /sys/fs/selinux/load.
 *
 *   sepatch <in.policy> <out.policy> <rules-file>
 *
 * Rules are the module's sepolicy.rule format, one per line:
 *   allow <source> <target> <class> <perm>
 *   allow <source> <target> <class> { <perm> <perm> ... }
 * Only plain types (no attributes, no sets) are needed for it. */
#include <sepol/policydb/policydb.h>
#include <sepol/policydb/avtab.h>
#include <sepol/policydb/hashtab.h>
#include <sepol/policydb/polcaps.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int add_perm(policydb_t *p, const char *s, const char *t, const char *c, const char *perm) {
    type_datum_t *src = hashtab_search(p->p_types.table, (hashtab_key_t)s);
    type_datum_t *tgt = hashtab_search(p->p_types.table, (hashtab_key_t)t);
    class_datum_t *cls = hashtab_search(p->p_classes.table, (hashtab_key_t)c);
    if (!src || !tgt || !cls) {
        fprintf(stderr, "unknown name in: %s %s %s\n", s, t, c);
        return -1;
    }
    perm_datum_t *pd = hashtab_search(cls->permissions.table, (hashtab_key_t)perm);
    if (!pd && cls->comdatum) pd = hashtab_search(cls->comdatum->permissions.table, (hashtab_key_t)perm);
    if (!pd) {
        fprintf(stderr, "unknown permission %s in class %s\n", perm, c);
        return -1;
    }
    avtab_key_t key;
    memset(&key, 0, sizeof(key));
    key.source_type = src->s.value;
    key.target_type = tgt->s.value;
    key.target_class = cls->s.value;
    key.specified = AVTAB_ALLOWED;
    const uint32_t bit = 1U << (pd->s.value - 1);
    avtab_ptr_t node = avtab_search_node(&p->te_avtab, &key);
    if (node) {
        const int had = (node->datum.data & bit) != 0;
        node->datum.data |= bit;
        printf("%s %s %s %s %s %s\n", had ? "had " : "added", "allow", s, t, c, perm);
        return 0;
    }
    avtab_datum_t d;
    memset(&d, 0, sizeof(d));
    d.data = bit;
    if (avtab_insert(&p->te_avtab, &key, &d)) {
        fprintf(stderr, "avtab_insert failed\n");
        return -1;
    }
    printf("added allow %s %s %s %s (new entry)\n", s, t, c, perm);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <in.policy> <out.policy> <rules-file>\n", argv[0]);
        return 2;
    }
    /* The device's policy may enable capabilities newer than this libsepol,
     * which then refuses the whole file. Mask those bits for the read and put
     * them back in the output: nothing below touches capabilities. */
    FILE *raw = fopen(argv[1], "rb");
    if (!raw) { perror(argv[1]); return 1; }
    fseek(raw, 0, SEEK_END);
    long size = ftell(raw);
    fseek(raw, 0, SEEK_SET);
    unsigned char *buf = malloc(size);
    if (!buf || fread(buf, 1, size, raw) != (size_t)size) { fprintf(stderr, "short read\n"); return 1; }
    fclose(raw);
    uint32_t len;
    memcpy(&len, buf + 4, 4);
    const long caps = 8 + len + 16;          /* magic, len, string, version, config, sym_num, ocon_num */
    uint32_t count, startbit;
    memcpy(&count, buf + caps + 8, 4);
    memcpy(&startbit, buf + caps + 12, 4);
    uint64_t map = 0, unknown = 0;
    if (count == 1 && startbit == 0) {
        memcpy(&map, buf + caps + 16, 8);
        for (int b = 0; b < 64; ++b)
            if ((map >> b & 1) && sepol_polcap_getname(b) == NULL) unknown |= 1ULL << b;
        uint64_t masked = map & ~unknown;
        memcpy(buf + caps + 16, &masked, 8);
        printf("policy capabilities 0x%llx, unknown to this libsepol 0x%llx\n",
               (unsigned long long)map, (unsigned long long)unknown);
    }
    FILE *in = fmemopen(buf, size, "rb");
    if (!in) { perror("fmemopen"); return 1; }
    struct policy_file pf;
    policy_file_init(&pf);
    pf.type = PF_USE_STDIO;
    pf.fp = in;
    policydb_t p;
    if (policydb_init(&p)) return 1;
    if (policydb_read(&p, &pf, 0)) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    fclose(in);
    printf("policy version %u\n", p.policyvers);

    FILE *rules = fopen(argv[3], "r");
    if (!rules) { perror(argv[3]); return 1; }
    char line[1024];
    int rc = 0;
    while (fgets(line, sizeof(line), rules)) {
        char *tok[64];
        int n = 0;
        for (char *w = strtok(line, " \t\r\n{}"); w && n < 64; w = strtok(NULL, " \t\r\n{}")) tok[n++] = w;
        if (n == 0 || tok[0][0] == '#') continue;
        if (strcmp(tok[0], "allow") != 0 || n < 5) { fprintf(stderr, "unsupported: %s\n", tok[0]); rc = 1; continue; }
        for (int i = 4; i < n; ++i)
            if (add_perm(&p, tok[1], tok[2], tok[3], tok[i])) rc = 1;
    }
    fclose(rules);
    if (rc) return rc;

    FILE *out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }
    pf.fp = out;
    if (policydb_write(&p, &pf)) { fprintf(stderr, "cannot write\n"); return 1; }
    fclose(out);
    policydb_destroy(&p);
    if (unknown) {
        FILE *fix = fopen(argv[2], "r+b");
        uint64_t m;
        if (!fix || fseek(fix, caps + 16, SEEK_SET) || fread(&m, 8, 1, fix) != 1) { fprintf(stderr, "cannot restore caps\n"); return 1; }
        m |= unknown;
        if (fseek(fix, caps + 16, SEEK_SET) || fwrite(&m, 8, 1, fix) != 1) { fprintf(stderr, "cannot restore caps\n"); return 1; }
        fclose(fix);
        printf("restored capabilities: 0x%llx\n", (unsigned long long)m);
    }
    return 0;
}
