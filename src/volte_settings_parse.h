#ifndef VOLTE_SETTINGS_PARSE_H
#define VOLTE_SETTINGS_PARSE_H

#include <stddef.h>

/* Pure parsers for the settings UI's two files and for ims_rrm's sip log. No QNX or daemon
 * dependencies, so tests/test_volte_settings.c can feed them text on the host. */

/* "mode=sip-learned\nrefresh=1\nrequest=1234\n"; "knob=<name>:<0|1>" is copied to knob as is, and
   "action=restart" or "action=fetch" (restart IMS, download the carrier list) to action. */
int volte_settings_parse_request(const char *text, char *mode, size_t mode_len,
                                 char *request, size_t request_len, int *refresh, int *debug,
                                 char *knob, size_t knob_len, char *action, size_t action_len);

/* The operator's own domain from the registered IMPU: P-Associated-URI, <uri>, or the
 * From:/To: of the reg-event NOTIFY. Domains ending in .3gppnetwork.org and
 * anonymous.invalid are skipped; out stays empty when there is none. */
int volte_settings_parse_learned_domain(const char *slog, char *out, size_t out_len);

/* Last outgoing call. result: "connected", "503 (1:223)", "failed (code)", "in progress",
 * "no call seen" or "unknown"; uri: the R-URI domain ("tel:" for a tel URI). The number is never
 * copied out: id is a hash of the call's createOutDialog line, 0 when there is no call. */
typedef struct
{
    char result[48];
    char uri[64];
    int code;          /* final SIP code from onDialogEnded, 0 if none */
    int connected;     /* the dialog reached state 4 */
    int in_progress;
    unsigned long id;
} volte_call_t;

void volte_settings_parse_last_call(const char *slog, volte_call_t *call);

/* Letter for a mode in the failed list: 't' tel, 'i' sip-imsi, 'l' sip-learned, 0 otherwise. */
char volte_mode_letter(const char *mode);
const char *volte_mode_from_letter(char letter);

/* Next mode to try, skipping modes whose letter is in failed. Order: tel, sip-learned (only when
   learned_differs), sip-imsi. NULL when all candidates failed. */
const char *volte_auto_next(const char *failed, int learned_differs);

/* 1 when the network refused the call's address: never connected and code 503, what EE and
   Telekom DE sent for a dial format they don't route. 403/404/484 are left out: a misdialled
   number gets those too. Our own 580 (preconditions), 486 and 487 are not refusals. */
int volte_call_refused(const volte_call_t *call);

/* The dial mode a call used, from its R-URI: "tel" for "tel:", "sip-imsi" when the domain equals
   imsi_domain, "sip-learned" for any other domain, NULL when unknown/empty. */
const char *volte_call_mode(const volte_call_t *call, const char *imsi_domain);

/* APN of one service in a carrier config XML: the <APN> text inside the <ServiceConfiguration>
   whose <Name> is exactly service. out is empty and 0 is returned when there is none. */
int volte_settings_parse_apn(const char *xml, const char *service, char *out, size_t out_len);

#endif /* VOLTE_SETTINGS_PARSE_H */
