/* Run with `make test`. Needs only libxml2 and libcurl on the build host. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "carrier_lookup.h"
#include "carrier_profile.h"
#include "ims_carrier_db.h"

#define VOD_ID 284
#define BT_ID 27
#define COOP_ID (AOSP_ID_BASE + 2452)
#define AMP_ID (AOSP_ID_BASE + 3000)
#define BAD_APN_ID (AOSP_ID_BASE + 3001)

static const char k_stock[] =
    "<?xml version=\"1.0\"?>\n<rows>\n"
    "  <row><carrier_id>27</carrier_id><carrier_name>BT Italy</carrier_name><vendor_id>1</vendor_id>"
    "<vendor_id>2</vendor_id><mcc>222</mcc><mnc>010</mnc><efgid>04</efgid></row>\n"
    "  <row><carrier_id>284</carrier_id><carrier_name>Vodafone Italy</carrier_name><vendor_id>3</vendor_id>"
    "<mcc>222</mcc><mnc>010</mnc><efgid></efgid></row>\n"
    "  <row><carrier_id>50</carrier_id><carrier_name>Limited</carrier_name><mcc>208</mcc><mnc>009</mnc><efgid></efgid>"
    "<imsirange><imsimin>1</imsimin><imsimax>2</imsimax></imsirange></row>\n"
    "  <row><carrier_id>60</carrier_id><carrier_name>Login</carrier_name><mcc>222</mcc><mnc>060</mnc><efgid></efgid></row>\n"
    "  <row><carrier_id>61</carrier_id><carrier_name>Two Networks</carrier_name><mcc>222</mcc><mnc>061</mnc><efgid></efgid></row>\n"
    "  <row><carrier_id>61</carrier_id><carrier_name>Two Networks</carrier_name><mcc>222</mcc><mnc>062</mnc><efgid></efgid></row>\n"
    "  <row><carrier_id>62</carrier_id><carrier_name>Unusable</carrier_name><mcc>222</mcc><mnc>063</mnc><efgid></efgid></row>\n"
    "  <row><carrier_id>71</carrier_id><carrier_name>Own APN Unusable</carrier_name><mcc>222</mcc><mnc>071</mnc><efgid></efgid></row>\n"
    "  <row><carrier_id>72</carrier_id><carrier_name>Odd Code</carrier_name><mcc>222</mcc><mnc>072</mnc><efgid></efgid></row>\n"
    "  <row><carrier_id>72</carrier_id><carrier_name>Odd Code</carrier_name><mcc>222</mcc><mnc>073</mnc><efgid></efgid></row>\n"
    "  <row><carrier_id>72</carrier_id><carrier_name>Odd Code</carrier_name><mcc>222</mcc><mnc>074</mnc><efgid></efgid></row>\n"
    "  <row><carrier_id>75</carrier_id><carrier_name>Two Plans</carrier_name><mcc>222</mcc><mnc>075</mnc><efgid></efgid></row>\n"
    "  <row><carrier_id>24</carrier_id><carrier_name>CDMA</carrier_name><mcc>SID</mcc><mnc>5F5</mnc><efgid></efgid></row>\n"
    "</rows>\n";

static const char k_apns[] =
    "<apns version=\"8\">"
    "<apn carrier=\"WEB\" carrier_id=\"2452\" mcc=\"222\" mnc=\"53\" apn=\"internet.coopvoce.it\" type=\"default,ia,supl\"/>"
    "<apn carrier=\"MMS\" carrier_id=\"2452\" mcc=\"222\" mnc=\"53\" apn=\"message.coopvoce.it\" type=\"mms\"/>"
    "<apn carrier=\"Amp\" carrier_id=\"3000\" apn=\"a.net\" type=\"default\" user=\"u&amp;1\" password=\"p&lt;2\"/>"
    "<apn carrier=\"Bad\" carrier_id=\"3001\" apn=\"bad apn&amp;\" type=\"default\"/>"
    "<apn carrier=\"Vod MVNO\" mcc=\"222\" mnc=\"10\" mvno_type=\"gid\" mvno_match_data=\"04\" apn=\"mvno.net\" type=\"default\"/>"
    "<apn carrier=\"Vod\" carrier_id=\"29\" mcc=\"222\" mnc=\"10\" apn=\"vod.android\" type=\"default\"/>"
    "<apn carrier=\"Own\" carrier_id=\"3010\" mcc=\"222\" mnc=\"71\" apn=\"own.v6\" protocol=\"IPV6\" type=\"default\"/>"
    "<apn carrier=\"Placeholder\" mcc=\"222\" mnc=\"63\" apn=\"default\" type=\"default\"/>"
    "<apn carrier=\"Other\" mcc=\"222\" mnc=\"71\" apn=\"borrowed.net\" type=\"default\"/>"
    "<apn carrier=\"Someone\" mcc=\"222\" mnc=\"74\" apn=\"someone.net\" type=\"default\"/>"
    "<apn carrier=\"Login\" mcc=\"222\" mnc=\"60\" apn=\"fresh.net\" type=\"default\"/>"
    "<apn carrier=\"A\" mcc=\"222\" mnc=\"61\" apn=\"first.net\" type=\"default\"/>"
    "<apn carrier=\"B\" mcc=\"222\" mnc=\"62\" apn=\"second.net\" type=\"default\"/>"
    "<apn carrier=\"IPv6\" mcc=\"222\" mnc=\"63\" apn=\"v6.net\" protocol=\"IPV6\" type=\"default\"/>"
    "<apn carrier=\"5G\" mcc=\"222\" mnc=\"63\" apn=\"nr.net\" network_type_bitmask=\"20\" type=\"default\"/>"
    "<apn carrier=\"Virtual\" mcc=\"222\" mnc=\"99\" mvno_type=\"spn\" apn=\"virtual.net\" type=\"default\"/>"
    "<apn carrier=\"Host\" mcc=\"222\" mnc=\"99\" apn=\"host.net\" type=\"default\"/>"
    "<apn carrier=\"Plan 1\" carrier_id=\"3011\" mcc=\"222\" mnc=\"75\" apn=\"web1\" type=\"default,ia,supl\"/>"
    "<apn carrier=\"Plan 2\" carrier_id=\"3011\" mcc=\"222\" mnc=\"75\" apn=\"web2\" type=\"default,ia,supl\"/>"
    "</apns>";

static const char k_default_config[] =
    "<?xml version=\"1.0\"?>\n"
    "<CarrierConfiguration xmlns=\"http://www.rim.com/CarrierConfig/2012-01\">"
    "<CarrierDataConfiguration><Name>BlackBerry Default</Name><CarrierID>0</CarrierID>"
    "<ServiceConfiguration><Name>internet</Name><RadioAccessTechnology><APN></APN></RadioAccessTechnology></ServiceConfiguration>"
    "<ServiceConfigurationAlias><Name>ims</Name><Reference>internet</Reference></ServiceConfigurationAlias>"
    "<ServiceConfigurationAlias><Name>mms</Name><Reference>internet</Reference></ServiceConfigurationAlias>"
    "<ServiceConfigurationAlias><Name>vvm</Name><Reference>internet</Reference></ServiceConfigurationAlias>"
    "</CarrierDataConfiguration></CarrierConfiguration>\n";

#define CONFIG_HEAD \
    "<?xml version=\"1.0\"?>\n" \
    "<CarrierConfiguration xmlns=\"http://www.rim.com/CarrierConfig/2012-01\"><CarrierDataConfiguration><Name>Stock</Name>"
#define CONFIG_TAIL "</CarrierDataConfiguration></CarrierConfiguration>\n"
#define INTERNET_SERVICE \
    "<ServiceConfiguration><Name>internet</Name><Description>d</Description><AlwaysOn>true</AlwaysOn>" \
    "<RadioAccessTechnology><APN>mobile.vodafone.it</APN></RadioAccessTechnology></ServiceConfiguration>"

/* As Vodafone Italy ships: an attach bearer named ims, with no APN and no IMS settings. */
static const char k_config_attach_bearer[] = CONFIG_HEAD
    "<ServiceConfiguration><Name>ims</Name><LTEAttachService>true</LTEAttachService>"
    "<RadioAccessTechnology><APN></APN></RadioAccessTechnology></ServiceConfiguration>"
    INTERNET_SERVICE CONFIG_TAIL;

/* As the Verizon resellers ship: their own IMS APN, but no IMS settings. */
static const char k_config_own_apn[] = CONFIG_HEAD
    INTERNET_SERVICE
    "<ServiceConfiguration><Name>ims</Name><RadioAccessTechnology><APN>vzwims</APN></RadioAccessTechnology></ServiceConfiguration>"
    CONFIG_TAIL;

/* As AT&T ships: a real IMS service, which must not be touched. */
static const char k_config_real_ims[] = CONFIG_HEAD
    INTERNET_SERVICE
    "<ServiceConfiguration><Name>ims</Name><RadioAccessTechnology><APN>carrier-ims</APN></RadioAccessTechnology>"
    "<IMSConfiguration><RequestPCSCFViaPCO>true</RequestPCSCFViaPCO></IMSConfiguration></ServiceConfiguration>"
    CONFIG_TAIL;

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static char *read_text(const char *path)
{
    static char buf[1024 * 1024];
    FILE *f = fopen(path, "r");
    assert(f);
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';
    return buf;
}

/* Enough filler carriers to pass the guard against a truncated download. */
static char *build_textpb(void)
{
    static char buf[64 * 1024];
    size_t n = (size_t)snprintf(buf, sizeof(buf),
        "version: 1\n"
        "carrier_id {\n  canonical_id: 2452\n  carrier_name: \"WEB CoopVoce\"\n"
        "  carrier_attribute {\n    mccmnc_tuple: \"22253\"\n  }\n}\n"
        "carrier_id {\n  canonical_id: 2453\n  carrier_name: \"Vodafone Dup\"\n"
        "  carrier_attribute {\n    mccmnc_tuple: \"22210\"\n  }\n}\n"
        "carrier_id {\n  canonical_id: 3000\n  carrier_name: \"A&B <Mobile>\"\n"
        "  carrier_attribute {\n    mccmnc_tuple: \"22298\"\n    gid1: \"ab\"\n    gid1: \"cd\"\n  }\n}\n"
        "carrier_id {\n  canonical_id: 3001\n  carrier_name: \"Bad APN\"\n"
        "  carrier_attribute {\n    mccmnc_tuple: \"22297\"\n  }\n}\n"
        "carrier_id {\n  canonical_id: 3002\n  carrier_name: \"Host Network\"\n"
        "  carrier_attribute {\n    mccmnc_tuple: \"22299\"\n  }\n}\n"
        "carrier_id {\n  canonical_id: 3003\n  carrier_name: \"SPN MVNO\"\n"
        "  carrier_attribute {\n    mccmnc_tuple: \"22296\"\n    spn: \"mvno\"\n  }\n}\n"
        "carrier_id {\n  canonical_id: 3004\n  carrier_name: \"Plain and Limited\"\n"
        "  carrier_attribute {\n    mccmnc_tuple: \"22295\"\n    imsi_prefix_xpattern: \"22295123\"\n  }\n"
        "  carrier_attribute {\n    mccmnc_tuple: \"22294\"\n  }\n}\n"
        "carrier_id {\n  canonical_id: 3011\n  carrier_name: \"Two Plans\"\n"
        "  carrier_attribute {\n    mccmnc_tuple: \"22275\"\n  }\n}\n"
        "carrier_id {\n  canonical_id: 3010\n  carrier_name: \"Own APN Unusable\"\n"
        "  carrier_attribute {\n    mccmnc_tuple: \"22271\"\n  }\n}\n"
        "carrier_id {\n  canonical_id: 3005\n  carrier_name: \"GID on Stock Network\"\n"
        "  carrier_attribute {\n    mccmnc_tuple: \"22210\"\n    gid1: \"ee\"\n  }\n}\n");

    for (int i = 0; i < 120; i++)
    {
        n += (size_t)snprintf(buf + n, sizeof(buf) - n,
            "carrier_id {\n  canonical_id: %d\n  carrier_name: \"Filler\"\n"
            "  carrier_attribute {\n    mccmnc_tuple: \"999%03d\"\n  }\n}\n", 4000 + i, i);
    }
    return buf;
}

int main(void)
{
    char dir[] = "/tmp/carrier-test-XXXXXX";
    assert(mkdtemp(dir));
    assert(chdir(dir) == 0);
    assert(mkdir("config", 0755) == 0);

    write_text("stock.xml", k_stock);
    write_text("config/0.xml", k_default_config);
    write_text("config/284.xml", k_config_attach_bearer);
    write_text("config/729.xml", k_config_own_apn);
    write_text("config/50.xml", k_config_real_ims);

    /* A guard, not a detail: a truncated download must never replace the list. */
    assert(ims_carrier_db_generate("carrier_id {\n}\n", k_apns, strlen(k_apns), "stock.xml", "db.xml", "apns.tsv") == -1);
    assert(access("db.xml", F_OK) != 0);

    assert(ims_carrier_db_generate(build_textpb(), k_apns, strlen(k_apns), "stock.xml", "db.xml", "apns.tsv") == 0);

    /* Stock rows stay byte-identical, including repeated vendor_ids and limited rows. */
    const char *db = read_text("db.xml");
    assert(strncmp(db, k_stock, strlen(k_stock) - strlen("</rows>\n")) == 0);
    assert(strstr(db, "A&amp;B &lt;Mobile&gt;"));
    assert(!strstr(db, "Vodafone Dup"));
    assert(!strstr(db, "SPN MVNO"));
    assert(strstr(db, "<mnc>053</mnc>"));

    /* The GID decides, and the network's own row catches everything else. */
    assert(ims_find_carrier_id("db.xml", "222", "10", "04ff") == BT_ID);
    assert(ims_find_carrier_id("db.xml", "222", "10", "ffff") == VOD_ID);
    assert(ims_find_carrier_id("db.xml", "222", "10", NULL) == VOD_ID);
    assert(ims_find_carrier_id("db.xml", "222", "010", "") == VOD_ID);
    assert(ims_find_carrier_id("db.xml", "222", "53", "434f4f50ffffffffff") == COOP_ID);

    /* A new GID row must not take a SIM away from the stock config it has today. */
    assert(ims_find_carrier_id("db.xml", "222", "10", "ee12") == VOD_ID);

    /* A row limited to an IMSI range or a GID isn't the network. */
    assert(ims_find_carrier_id("db.xml", "208", "09", NULL) == -1);
    assert(ims_find_carrier_id("db.xml", "222", "98", "ab12") == AMP_ID);
    assert(ims_find_carrier_id("db.xml", "222", "98", "ef") == -1);
    assert(ims_find_carrier_id("db.xml", "222", "96", NULL) == -1);
    assert(ims_find_carrier_id("db.xml", "222", "95", NULL) == -1);
    assert(ims_find_carrier_id("db.xml", "222", "94", NULL) == AOSP_ID_BASE + 3004);

    /* New carrier: stock default plus Android APNs, with services ahead of the aliases. */
    assert(ims_build_carrier_xml(COOP_ID, "config", "apns.tsv", "coop.xml") == 0);
    const char *coop = read_text("coop.xml");
    assert(strstr(coop, "<Name>WEB CoopVoce</Name>"));
    assert(strstr(coop, "<APN>internet.coopvoce.it</APN>"));
    assert(strstr(coop, "<APN>message.coopvoce.it</APN>"));
    assert(strstr(coop, "<APN>ims</APN>"));
    assert(strstr(coop, "<Name>vvm</Name>"));

    /* The aliases these services replace are gone, and the services come before the rest. */
    assert(strstr(coop, "<Name>ims</Name>") < strstr(coop, "<Name>vvm</Name>"));
    assert(strstr(coop, "<Name>mms</Name>") < strstr(coop, "<Name>ims</Name>"));
    assert(!strstr(strstr(coop, "<Name>ims</Name>") + 1, "<Name>ims</Name>"));
    assert(!strstr(strstr(coop, "<Name>mms</Name>") + 1, "<Name>mms</Name>"));

    /* One namespace declaration: the data service's parser never saw a copied one. */
    assert(!strstr(strstr(coop, "xmlns=") + 1, "xmlns="));

    /* An unchanged profile keeps its timestamp. */
    struct stat before, after;
    assert(stat("coop.xml", &before) == 0);
    sleep(1);
    assert(ims_build_carrier_xml(COOP_ID, "config", "apns.tsv", "coop.xml") == 0);
    assert(stat("coop.xml", &after) == 0);
    assert(before.st_mtime == after.st_mtime);

    /* Credentials arrive through XML, so they must be escaped going back out. */
    assert(ims_build_carrier_xml(AMP_ID, "config", "apns.tsv", "amp.xml") == 0);
    const char *amp = read_text("amp.xml");
    assert(strstr(amp, "<Username>u&amp;1</Username>"));
    assert(strstr(amp, "<Password>p&lt;2</Password>"));

    /* No usable APN is left empty for the user, never guessed, and can still be edited. */
    assert(ims_build_carrier_xml(BAD_APN_ID, "config", "apns.tsv", "bad.xml") == 0);
    const char *bad = read_text("bad.xml");
    assert(strstr(bad, "<APN/>") || strstr(bad, "<APN></APN>"));
    assert(!strstr(bad, "bad apn"));
    assert(strstr(bad, "<APN>ims</APN>"));
    assert(strstr(bad, "<AppModifiable>true</AppModifiable>"));

    /* Once the profile is linked over its stock path, a rebuild must not stack the APNs. */
    char link_path[64];
    snprintf(link_path, sizeof(link_path), "config/%d.xml", AMP_ID);
    assert(symlink("../amp.xml", link_path) == 0);
    assert(ims_build_carrier_xml(AMP_ID, "config", "apns.tsv", "amp.xml") == 0);
    amp = read_text("amp.xml");
    assert(!strstr(strstr(amp, "<Authentication>") + 1, "<Authentication>"));

    /* The network's own APN is used, not a virtual operator's on the same network. */
    assert(ims_build_carrier_xml(AOSP_ID_BASE + 3002, "config", "apns.tsv", "host.xml") == 0);
    assert(strstr(read_text("host.xml"), "<APN>host.net</APN>"));

    /* A stock bearer named ims is replaced, and internet takes over the LTE attach. */
    assert(ims_build_carrier_xml(VOD_ID, "config", "apns.tsv", "vod.xml") == 0);
    const char *vod = read_text("vod.xml");
    assert(strstr(vod, "<APN>vod.android</APN>"));
    assert(!strstr(vod, "mobile.vodafone.it"));
    assert(strstr(vod, "<Name>Stock</Name>"));
    assert(strstr(vod, "<APN>ims</APN>"));
    assert(strstr(vod, "<RequestPCSCFViaPCO>"));
    const char *internet = strstr(vod, "<Name>internet</Name>");
    assert(internet && strstr(internet, "<LTEAttachService>true</LTEAttachService>"));
    assert(strstr(vod, "<LTEAttachService>") == strstr(internet, "<LTEAttachService>"));
    assert(strstr(internet, "<AlwaysOn>") < strstr(internet, "<AppModifiable>"));
    assert(strstr(internet, "<AppModifiable>") < strstr(internet, "<LTEAttachService>"));
    assert(!strstr(strstr(vod, "<Name>ims</Name>") + 1, "<Name>ims</Name>"));

    /* The Android APN replaces the stock one, and the stock login leaves with it. */
    write_text("config/60.xml", CONFIG_HEAD
        "<ServiceConfiguration><Name>internet</Name><RadioAccessTechnology><APN>old.net</APN>"
        "<Authentication><PAP><Username>old</Username><Password>old</Password></PAP></Authentication>"
        "</RadioAccessTechnology></ServiceConfiguration>" CONFIG_TAIL);
    assert(ims_build_carrier_xml(60, "config", "apns.tsv", "login.xml") == 0);
    const char *login = read_text("login.xml");
    assert(strstr(login, "<APN>fresh.net</APN>"));
    assert(!strstr(login, "Authentication"));

    /* Stock stays when Android disagrees with itself, has only unusable APNs, or isn't exact. */
    const char *stock_apn = CONFIG_HEAD
        "<ServiceConfiguration><Name>internet</Name><RadioAccessTechnology><APN>stock.net</APN>"
        "</RadioAccessTechnology></ServiceConfiguration>" CONFIG_TAIL;
    write_text("config/61.xml", stock_apn);
    write_text("config/62.xml", stock_apn);
    write_text("config/27.xml", stock_apn);
    assert(ims_build_carrier_xml(61, "config", "apns.tsv", "conflict.xml") == 0);
    assert(strstr(read_text("conflict.xml"), "<APN>stock.net</APN>"));
    assert(ims_build_carrier_xml(62, "config", "apns.tsv", "unusable.xml") == 0);
    assert(strstr(read_text("unusable.xml"), "<APN>stock.net</APN>"));
    write_text("config/71.xml", stock_apn);
    write_text("config/72.xml", stock_apn);
    assert(ims_build_carrier_xml(71, "config", "apns.tsv", "own-v6.xml") == 0);
    assert(strstr(read_text("own-v6.xml"), "<APN>stock.net</APN>"));
    assert(ims_build_carrier_xml(72, "config", "apns.tsv", "odd.xml") == 0);
    assert(strstr(read_text("odd.xml"), "<APN>stock.net</APN>"));

    /* Android lists one APN per plan: a stock APN among them stays, any other stock APN is replaced. */
    write_text("config/75.xml", CONFIG_HEAD
        "<ServiceConfiguration><Name>internet</Name><RadioAccessTechnology><APN>WEB2</APN>"
        "<Authentication><PAP><Username>s</Username><Password>s</Password></PAP></Authentication>"
        "</RadioAccessTechnology></ServiceConfiguration>" CONFIG_TAIL);
    assert(ims_build_carrier_xml(75, "config", "apns.tsv", "plans.xml") == 0);
    assert(strstr(read_text("plans.xml"), "<APN>WEB2</APN>"));
    assert(strstr(read_text("plans.xml"), "<Username>s</Username>"));
    write_text("config/75.xml", stock_apn);
    assert(ims_build_carrier_xml(75, "config", "apns.tsv", "plans.xml") == 0);
    assert(strstr(read_text("plans.xml"), "<APN>web1</APN>"));
    assert(ims_build_carrier_xml(BT_ID, "config", "apns.tsv", "gid.xml") == 0);
    assert(strstr(read_text("gid.xml"), "<APN>stock.net</APN>"));

    /* A carrier's own IMS APN survives the replacement, with no attach move it didn't ask for. */
    assert(ims_build_carrier_xml(729, "config", "apns.tsv", "own.xml") == 0);
    const char *own = read_text("own.xml");
    assert(strstr(own, "<APN>vzwims</APN>"));
    assert(strstr(own, "<RequestPCSCFViaPCO>"));
    assert(!strstr(own, "<LTEAttachService>"));

    /* A config that forbids APN changes is opened up, in the place stock configs put it. */
    write_text("config/48.xml", CONFIG_HEAD
        "<ServiceConfiguration><Name>internet</Name><AppModifiable>false</AppModifiable></ServiceConfiguration>"
        CONFIG_TAIL);
    assert(ims_build_carrier_xml(48, "config", "apns.tsv", "locked.xml") == 0);
    assert(strstr(read_text("locked.xml"), "<AppModifiable>true</AppModifiable>"));

    /* A real IMS service stays exactly as shipped. */
    assert(ims_build_carrier_xml(50, "config", "apns.tsv", "real.xml") == 0);
    const char *real = read_text("real.xml");
    assert(strstr(real, "<APN>carrier-ims</APN>"));
    assert(!strstr(real, "ProtectRouting"));

    /* A carrier nobody knows the APN of still gets a profile. */
    assert(ims_build_carrier_xml(999, "config", "apns.tsv", "unknown.xml") == 0);

    /* Only a missing stock default leaves nothing to build from. */
    assert(unlink("config/0.xml") == 0);
    assert(ims_build_carrier_xml(999, "config", "apns.tsv", "none.xml") == -1);

    printf("carrier tests passed\n");
    return 0;
}
