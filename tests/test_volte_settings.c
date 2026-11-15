/* Host tests for the settings app's log parsers and the store-only zip writer. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "volte_settings_parse.h"
#include "zip_writer.h"

static void test_request(void)
{
    char mode[16], request[32], knob[32], action[16];
    int refresh = 0, debug = 0;

    assert(volte_settings_parse_request("mode=sip-learned\nrefresh=1\nrequest=42\n",
                                        mode, sizeof(mode), request, sizeof(request),
                                        &refresh, &debug, knob, sizeof(knob), action, sizeof(action)) == 1);
    assert(strcmp(mode, "sip-learned") == 0);
    assert(strcmp(request, "42") == 0);
    assert(refresh == 1 && debug == 0);

    assert(volte_settings_parse_request("debug=1\nrequest=7\n", mode, sizeof(mode),
                                        request, sizeof(request), &refresh, &debug, knob, sizeof(knob), action, sizeof(action)) == 1);
    assert(strcmp(mode, "") == 0);
    assert(strcmp(request, "7") == 0);
    assert(debug == 1 && refresh == 0);

    assert(volte_settings_parse_request("knob=rtcp-bw:1\nrequest=5\n", mode, sizeof(mode),
                                        request, sizeof(request), &refresh, &debug, knob,
                                        sizeof(knob), action, sizeof(action)) == 1);
    assert(strcmp(knob, "rtcp-bw:1") == 0);
    assert(strcmp(mode, "") == 0);

    assert(volte_settings_parse_request("action=fetch\nrequest=9\n", mode, sizeof(mode), request,
                                        sizeof(request), &refresh, &debug, knob, sizeof(knob),
                                        action, sizeof(action)) == 1);
    assert(strcmp(action, "fetch") == 0 && strcmp(mode, "") == 0 && strcmp(knob, "") == 0);
    assert(volte_settings_parse_request("action=restart\nrequest=10\n", mode, sizeof(mode), request,
                                        sizeof(request), &refresh, &debug, knob, sizeof(knob),
                                        action, sizeof(action)) == 1);
    assert(strcmp(action, "restart") == 0);

    /* No request stamp: not a new request. */
    assert(volte_settings_parse_request("mode=tel\n", mode, sizeof(mode), request,
                                        sizeof(request), &refresh, &debug, knob, sizeof(knob), action, sizeof(action)) == 0);
    assert(volte_settings_parse_request(NULL, mode, sizeof(mode), request, sizeof(request),
                                        &refresh, &debug, knob, sizeof(knob), action, sizeof(action)) == 0);
}

static void test_learned_domain(void)
{
    const char *slog =
        "Nov 10 17:23:17.416 ims_rrm sip INVITE sip:<hidden>@ims.mnc001.mcc262.3gppnetwork.org SIP/2.0\n"
        "Nov 10 17:23:17.416 ims_rrm sip P-Associated-URI: <sip:+491234@ims.telekom.de>, <tel:+491234>\n"
        "Nov 10 17:23:17.416 ims_rrm sip <uri>sip:+491234@ims.telekom.de</uri>\n"
        "Nov 10 17:23:17.416 ims_rrm sip From: <sip:+491234@ims.telekom.de>;tag=x\n";
    char out[128];

    assert(volte_settings_parse_learned_domain(slog, out, sizeof(out)) == 1);
    assert(strcmp(out, "ims.telekom.de") == 0);

    /* Only an IMSI domain: used as the fallback, so the toggle works on carriers whose
       IMPU lives in ims.mnc...3gppnetwork.org. Anonymous is still skipped. */
    assert(volte_settings_parse_learned_domain(
               "From: <sip:262011234567890@ims.mnc001.mcc262.3gppnetwork.org>\n"
               "From: <sip:anonymous@anonymous.invalid>\n",
               out, sizeof(out)) == 1);
    assert(strcmp(out, "ims.mnc001.mcc262.3gppnetwork.org") == 0);
    assert(volte_settings_parse_learned_domain("", out, sizeof(out)) == 0);
    assert(volte_settings_parse_learned_domain(NULL, out, sizeof(out)) == 0);
}

static void test_last_call(void)
{
    const char *failed =
        "createOutDialog(): R-URI: sip:+491234@ims.telekom.de;user=phone\n"
        "SIP/2.0 100 Trying\n"
        "Reason: SIP;cause=503;text=\"Service Unavailable (1:223)\"\n"
        "onDialogStateChange() 0 => 4\n"
        "onDialogEnded(): call-id x, reason 5, code 503\n";
    const char *connected =
        "createOutDialog(): R-URI: tel:+391234\n"
        "onDialogStateChange() 0 => 0\n"
        "onDialogStateChange() 0 => 2\n"
        "onDialogStateChange() 2 => 4\n"
        "onDialogEnded(): call-id x, reason 3, code 16\n";
    volte_call_t call;

    volte_settings_parse_last_call(failed, &call);
    assert(strcmp(call.result, "503 (1:223)") == 0);
    assert(strcmp(call.uri, "ims.telekom.de") == 0);
    assert(call.code == 503 && !call.connected && !call.in_progress);

    volte_settings_parse_last_call(connected, &call);
    assert(strcmp(call.result, "connected") == 0);
    assert(strcmp(call.uri, "tel:") == 0);
    assert(call.connected && !call.in_progress);

    /* The reg-event SUBSCRIBE after a registration is not a call. */
    const char *subscribe_only =
        "sip.ua.client: SubscribeToRegInfo(): uri sip:+391234@ims.mnc053.mcc222.3gppnetwork.org\n"
        "sip.stack.dialogmanager: createOutDialog(): R-URI: sip:+391234@ims.mnc053.mcc222.3gppnetwork.org\n";
    volte_settings_parse_last_call(subscribe_only, &call);
    assert(strcmp(call.result, "no call seen") == 0);
    assert(call.id == 0);

    char after[512];
    snprintf(after, sizeof(after), "%s%s", connected, subscribe_only);
    volte_settings_parse_last_call(after, &call);
    assert(strcmp(call.result, "connected") == 0);
    assert(strcmp(call.uri, "tel:") == 0);

    volte_settings_parse_last_call("", &call);
    assert(strcmp(call.result, "no call seen") == 0);
    assert(call.uri[0] == '\0');
    assert(call.id == 0);

    volte_settings_parse_last_call("createOutDialog(): R-URI: tel:+10000000000\n", &call);
    assert(strcmp(call.result, "in progress") == 0);
    assert(call.in_progress);
}

static void test_call_id(void)
{
    const char *a = "10:00:01 createOutDialog(): R-URI: tel:+10000000000\n"
                    "onDialogEnded(): call-id x, reason 5, code 503\n";
    const char *b = "10:05:09 createOutDialog(): R-URI: tel:+10000000000\n"
                    "onDialogEnded(): call-id x, reason 5, code 503\n";
    volte_call_t c1, c2;

    volte_settings_parse_last_call(a, &c1);
    volte_settings_parse_last_call(a, &c2);
    assert(c1.id != 0);
    assert(c1.id == c2.id);

    volte_settings_parse_last_call(b, &c2);
    assert(c2.id != 0 && c2.id != c1.id);
}

static void test_auto(void)
{
    volte_call_t c;
    const char *imsi = "ims.mnc001.mcc262.3gppnetwork.org";

    assert(volte_mode_letter("tel") == 't');
    assert(volte_mode_letter("sip-imsi") == 'i');
    assert(volte_mode_letter("sip-learned") == 'l');
    assert(volte_mode_letter("auto") == 0 && volte_mode_letter(NULL) == 0);
    assert(strcmp(volte_mode_from_letter('l'), "sip-learned") == 0);
    assert(strcmp(volte_mode_from_letter('t'), "tel") == 0);
    assert(volte_mode_from_letter('x') == NULL);

    assert(strcmp(volte_auto_next("", 0), "tel") == 0);
    assert(strcmp(volte_auto_next("t", 1), "sip-learned") == 0);
    assert(strcmp(volte_auto_next("t", 0), "sip-imsi") == 0);
    assert(strcmp(volte_auto_next("tl", 1), "sip-imsi") == 0);
    assert(volte_auto_next("tli", 1) == NULL);
    assert(volte_auto_next("ti", 0) == NULL);

    memset(&c, 0, sizeof(c));
    c.code = 503;
    assert(volte_call_refused(&c) == 1);
    c.code = 404;
    assert(volte_call_refused(&c) == 0);
    c.code = 484;
    assert(volte_call_refused(&c) == 0);
    c.code = 403;
    assert(volte_call_refused(&c) == 0);
    c.code = 486;
    assert(volte_call_refused(&c) == 0);
    c.code = 487;
    assert(volte_call_refused(&c) == 0);
    c.code = 580;
    assert(volte_call_refused(&c) == 0);
    c.code = 503;
    c.connected = 1;
    assert(volte_call_refused(&c) == 0);
    c.connected = 0;
    c.code = 0;
    assert(volte_call_refused(&c) == 0);

    memset(&c, 0, sizeof(c));
    assert(volte_call_mode(&c, imsi) == NULL);
    snprintf(c.uri, sizeof(c.uri), "tel:");
    assert(strcmp(volte_call_mode(&c, imsi), "tel") == 0);
    snprintf(c.uri, sizeof(c.uri), "%s", imsi);
    assert(strcmp(volte_call_mode(&c, imsi), "sip-imsi") == 0);
    snprintf(c.uri, sizeof(c.uri), "ims.example.net");
    assert(strcmp(volte_call_mode(&c, imsi), "sip-learned") == 0);
}

static void test_apn(void)
{
    const char *xml =
        "<CarrierConfiguration>\n"
        "<ServiceConfiguration><Name>ims_admin</Name><RadioAccessTechnology><APN>admin.apn</APN>"
        "</RadioAccessTechnology></ServiceConfiguration>\n"
        "<ServiceConfiguration><Name>internet</Name><NetworkInterfaceConfiguration><Type>IPv4v6</Type>"
        "</NetworkInterfaceConfiguration><RadioAccessTechnology><APN>internet.v6.telekom</APN>"
        "</RadioAccessTechnology></ServiceConfiguration>\n"
        "<ServiceConfiguration><Name>ims</Name><NetworkInterfaceConfiguration><Type>IPv6</Type>"
        "</NetworkInterfaceConfiguration><RadioAccessTechnology><APN>ims</APN></RadioAccessTechnology>"
        "<Enabled>true</Enabled></ServiceConfiguration>\n"
        "<ServiceConfiguration><Name>wap</Name><RadioAccessTechnology><APN></APN></RadioAccessTechnology>"
        "</ServiceConfiguration>\n"
        "<ServiceConfiguration><Name>mms</Name><RadioAccessTechnology><APN/></RadioAccessTechnology>"
        "</ServiceConfiguration>\n"
        "<ServiceConfiguration><Name>tether</Name><RadioAccessTechnology><APN>\n  tether.apn \t</APN>"
        "</RadioAccessTechnology></ServiceConfiguration>\n"
        "</CarrierConfiguration>\n";
    char out[64];

    assert(volte_settings_parse_apn(xml, "internet", out, sizeof(out)) == 1);
    assert(strcmp(out, "internet.v6.telekom") == 0);
    /* ims_admin comes first: only the exact name counts. */
    assert(volte_settings_parse_apn(xml, "ims", out, sizeof(out)) == 1);
    assert(strcmp(out, "ims") == 0);
    assert(volte_settings_parse_apn(xml, "tether", out, sizeof(out)) == 1);
    assert(strcmp(out, "tether.apn") == 0);

    /* mms has <APN/>; the next service's (tether's) APN must not leak into it. */
    assert(volte_settings_parse_apn(xml, "mms", out, sizeof(out)) == 0 && out[0] == '\0');
    assert(volte_settings_parse_apn(xml, "wap", out, sizeof(out)) == 0 && out[0] == '\0');
    snprintf(out, sizeof(out), "stale");
    assert(volte_settings_parse_apn(xml, "dun", out, sizeof(out)) == 0 && out[0] == '\0');
    assert(volte_settings_parse_apn(xml, "internet", out, 8) == 0 && out[0] == '\0');
    assert(volte_settings_parse_apn(NULL, "ims", out, sizeof(out)) == 0 && out[0] == '\0');
}

static void test_zip(void)
{
    char dir[] = "/tmp/volte-zip-XXXXXX";
    char zip[] = "/tmp/volte-zip-test.zip";
    unsigned char buf[4096];
    FILE *f;
    size_t n;

    assert(mkdtemp(dir) != NULL);
    char path[512];
    snprintf(path, sizeof(path), "%s/a.txt", dir);
    f = fopen(path, "w");
    assert(f && fputs("hello", f) >= 0);
    fclose(f);
    snprintf(path, sizeof(path), "%s/b.txt", dir);
    f = fopen(path, "w");
    assert(f && fputs("world", f) >= 0);
    fclose(f);

    unlink(zip);
    assert(zip_directory(dir, zip) == 0);

    f = fopen(zip, "rb");
    assert(f);
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    assert(n > 22);
    assert(memcmp(buf, "PK\x03\x04", 4) == 0);

    /* End of central directory: count = 2. */
    for (size_t i = n; i >= 22; i--)
    {
        if (memcmp(buf + i - 22, "PK\x05\x06", 4) == 0)
        {
            assert(buf[i - 22 + 10] == 2 && buf[i - 22 + 11] == 0);
            break;
        }
        if (i == 22)
            assert(0);
    }

    unlink(zip);
    snprintf(path, sizeof(path), "%s/a.txt", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/b.txt", dir);
    unlink(path);
    rmdir(dir);
}

int main(void)
{
    test_request();
    test_learned_domain();
    test_last_call();
    test_call_id();
    test_auto();
    test_apn();
    test_zip();
    printf("test_volte_settings: OK\n");
    return 0;
}
