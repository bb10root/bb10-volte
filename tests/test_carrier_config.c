/* Host-side tests for the portable helpers in src/ims_carrier_config.c.
 * Build and run with: make test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "ims_carrier_config.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

static char g_tmp[256];

static void put(const char *name, const char *content)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_tmp, name);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(content, f);
    fclose(f);
}

static void rm(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_tmp, name);
    unlink(path);
}

static const char *base(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* carrierIds.xml rows in the device format; MNC is zero-padded to three digits there,
 * while the SIM HPLMN in PPS reports two ("03"). */
static const char CARRIER_IDS[] =
    "<?xml version=\"1.0\"?>\n<rows>\n"
    "  <row><carrier_id>162</carrier_id><carrier_name>Polkomtel</carrier_name>"
    "<mcc>260</mcc><mnc>001</mnc><efgid></efgid></row>\n"
    "  <row><carrier_id>149</carrier_id><carrier_name>Orange Poland</carrier_name>"
    "<vendor_id>183</vendor_id><vendor_id>504</vendor_id>"
    "<mcc>260</mcc><mnc>003</mnc><efgid></efgid></row>\n"
    "  <row><carrier_id>339</carrier_id><carrier_name>Vodafone UA</carrier_name>"
    "<mcc>255</mcc><mnc>01</mnc><efgid></efgid></row>\n"
    "</rows>\n";

static void test_carrier_id_lookup(void)
{
    char ids[512];
    snprintf(ids, sizeof(ids), "%s/carrierIds.xml", g_tmp);
    put("carrierIds.xml", CARRIER_IDS);

    CHECK(ims_get_carrier_id_from(ids, "260", "03") == 149);
    CHECK(ims_get_carrier_id_from(ids, "260", "003") == 149);
    CHECK(ims_get_carrier_id_from(ids, "260", "01") == 162);
    CHECK(ims_get_carrier_id_from(ids, "255", "01") == 339);
    CHECK(ims_get_carrier_id_from(ids, "260", "06") == -1);
    CHECK(ims_get_carrier_id_from(ids, "", "03") == -1);
    CHECK(ims_get_carrier_id_from("/nonexistent/carrierIds.xml", "260", "03") == -1);
}

static void test_find_profile(void)
{
    char out[512];

    /* Several profiles: the one matching the carrier id wins, whatever readdir order says. */
    put("carrier-36-volte.xml", "<x/>");
    put("carrier-404-volte.xml", "<x/>");
    put("carrier-149-volte.xml", "<x/>");
    CHECK(ims_find_carrier_profile(g_tmp, 149, out, sizeof(out)) == 0);
    CHECK(strcmp(base(out), "carrier-149-volte.xml") == 0);

    /* No match for the id: legacy behaviour, any carrier-*.xml is still used. */
    rm("carrier-149-volte.xml");
    CHECK(ims_find_carrier_profile(g_tmp, 149, out, sizeof(out)) == 0);
    CHECK(strncmp(base(out), "carrier-", 8) == 0);
    CHECK(strstr(base(out), ".xml") != NULL);

    /* Unknown carrier id (-1) behaves the same way. */
    CHECK(ims_find_carrier_profile(g_tmp, -1, out, sizeof(out)) == 0);

    /* Nothing to pick. */
    rm("carrier-36-volte.xml");
    rm("carrier-404-volte.xml");
    CHECK(ims_find_carrier_profile(g_tmp, 149, out, sizeof(out)) == -1);

    /* Prefix of a different id must not match ("carrier-1490-volte.xml" is not 149). */
    put("carrier-1490-volte.xml", "<x/>");
    put("carrier-149-volte.xml", "<x/>");
    CHECK(ims_find_carrier_profile(g_tmp, 149, out, sizeof(out)) == 0);
    CHECK(strcmp(base(out), "carrier-149-volte.xml") == 0);
    rm("carrier-1490-volte.xml");
    rm("carrier-149-volte.xml");
}

static void test_ir92_config(void)
{
    const char *def = "@ir92\nUsePreconditions:n:0\nSMS_Format::3GPP\n";
    const char *orange = "@ir92\nUsePreconditions:b:true\nSMS_Format::3GPP\n";
    char out[512];

    /* No override file: the default is used verbatim. */
    CHECK(ims_load_ir92_config(g_tmp, 149, def, out, sizeof(out)) == 0);
    CHECK(strcmp(out, def) == 0);

    /* Override for this carrier replaces the default. */
    put("ir92-149", orange);
    CHECK(ims_load_ir92_config(g_tmp, 149, def, out, sizeof(out)) == 0);
    CHECK(strcmp(out, orange) == 0);

    /* Other carriers keep the default. */
    CHECK(ims_load_ir92_config(g_tmp, 36, def, out, sizeof(out)) == 0);
    CHECK(strcmp(out, def) == 0);
    CHECK(ims_load_ir92_config(g_tmp, -1, def, out, sizeof(out)) == 0);
    CHECK(strcmp(out, def) == 0);

    /* An override that is not an @ir92 object is rejected and the default is used. */
    put("ir92-149", "UsePreconditions:b:true\n");
    CHECK(ims_load_ir92_config(g_tmp, 149, def, out, sizeof(out)) == 0);
    CHECK(strcmp(out, def) == 0);

    /* An override too large for the buffer is not truncated silently. */
    put("ir92-149", orange);
    char small[8];
    CHECK(ims_load_ir92_config(g_tmp, 149, def, small, sizeof(small)) == -1);
    rm("ir92-149");
}

static void test_ir92_applied(void)
{
    const char *expected = "@ir92\nUsePreconditions:b:true\nSMS_Format::3GPP\n";

    CHECK(ims_ir92_applied(expected, expected) == 1);
    /* PPS reorders attributes; order must not matter. */
    CHECK(ims_ir92_applied("@ir92\nSMS_Format::3GPP\nUsePreconditions:b:true\n", expected) == 1);
    /* Extra attributes in the actual object are fine. */
    CHECK(ims_ir92_applied("@ir92\nSMS_Format::3GPP\namr_modeset_list::7\nUsePreconditions:b:true\n",
                           expected) == 1);
    /* Missing or different value is not applied. */
    CHECK(ims_ir92_applied("@ir92\nSMS_Format::3GPP\n", expected) == 0);
    CHECK(ims_ir92_applied("@ir92\nUsePreconditions:n:0\nSMS_Format::3GPP\n", expected) == 0);
    /* A line must match whole: "UsePreconditions:b:true" is not satisfied by "...:b:trueX". */
    CHECK(ims_ir92_applied("@ir92\nUsePreconditions:b:trueX\nSMS_Format::3GPP\n", expected) == 0);
    /* Missing trailing newline on the last line still matches. */
    CHECK(ims_ir92_applied("@ir92\nSMS_Format::3GPP\nUsePreconditions:b:true", expected) == 1);
    CHECK(ims_ir92_applied(NULL, expected) == 0);
    CHECK(ims_ir92_applied("", expected) == 0);
}

int main(void)
{
    snprintf(g_tmp, sizeof(g_tmp), "/tmp/bb10-volte-test-XXXXXX");
    if (!mkdtemp(g_tmp)) { perror("mkdtemp"); return 2; }

    test_carrier_id_lookup();
    test_find_profile();
    test_ir92_config();
    test_ir92_applied();

    rm("carrierIds.xml");
    rmdir(g_tmp);

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
