#include "volte_settings_parse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '"')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '"'))
        *--end = '\0';
    return s;
}

int volte_settings_parse_request(const char *text, char *mode, size_t mode_len,
                                 char *request, size_t request_len, int *refresh, int *debug,
                                 char *knob, size_t knob_len, char *action, size_t action_len)
{
    char *copy, *line, *save = NULL;
    int have_request = 0;

    if (mode_len)
        mode[0] = '\0';
    if (request_len)
        request[0] = '\0';
    if (knob_len)
        knob[0] = '\0';
    if (action_len)
        action[0] = '\0';
    if (refresh)
        *refresh = 0;
    if (debug)
        *debug = 0;
    if (!text)
        return 0;

    copy = strdup(text);
    if (!copy)
        return 0;

    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    {
        char *eq = strchr(line, '=');
        char *key, *value;

        if (!eq)
            continue;
        *eq = '\0';
        key = trim(line);
        value = trim(eq + 1);

        if (strcmp(key, "mode") == 0)
            snprintf(mode, mode_len, "%s", value);
        else if (strcmp(key, "knob") == 0 && knob_len)
            snprintf(knob, knob_len, "%s", value);
        else if (strcmp(key, "action") == 0 && action_len)
            snprintf(action, action_len, "%s", value);
        else if (strcmp(key, "request") == 0)
        {
            snprintf(request, request_len, "%s", value);
            have_request = 1;
        }
        else if (strcmp(key, "refresh") == 0 && refresh)
            *refresh = atoi(value) != 0;
        else if (strcmp(key, "debug") == 0 && debug)
            *debug = atoi(value) != 0;
    }

    free(copy);
    return have_request;
}

/* First URI in the line. *is_3gpp says whether its domain is the IMSI-derived one. */
static int domain_from_line(const char *line, char *out, size_t out_len, int *is_3gpp)
{
    const char *p = strstr(line, "sip:");

    while (p)
    {
        const char *at = strchr(p, '@');
        if (at)
        {
            const char *d = at + 1;
            size_t n = strcspn(d, ">;\"' \t\r\n");
            if (n > 3 && n < out_len)
            {
                if (n == 17 && strncmp(d, "anonymous.invalid", 17) == 0)
                    ;
                else
                {
                    memcpy(out, d, n);
                    out[n] = '\0';
                    *is_3gpp = n > 16 && strncmp(d + n - 16, ".3gppnetwork.org", 16) == 0;
                    return 1;
                }
            }
        }
        p = strstr(p + 4, "sip:");
    }
    return 0;
}

int volte_settings_parse_learned_domain(const char *slog, char *out, size_t out_len)
{
    char *copy, *line, *save = NULL;
    char fallback[256] = "";
    int found = 0;

    if (!slog || !out || out_len == 0)
        return 0;
    out[0] = '\0';

    copy = strdup(slog);
    if (!copy)
        return 0;

    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    {
        char cand[256];
        int is_3gpp = 0;

        if (!(strstr(line, "P-Associated-URI") || strstr(line, "<uri") ||
              strstr(line, "From: <sip:") || strstr(line, "To: <sip:")))
            continue;
        if (!domain_from_line(line, cand, sizeof(cand), &is_3gpp))
            continue;

        if (!is_3gpp)
        {
            /* The operator's own domain wins over the IMSI-derived one. */
            snprintf(out, out_len, "%s", cand);
            found = 1;
            break;
        }
        if (fallback[0] == '\0')
            snprintf(fallback, sizeof(fallback), "%s", cand);
    }

    /* No operator domain in the log: the IMPU uses the IMSI domain, which is what the
       pre-2.33 sip: form sent. Still a valid "learned" domain for the toggle. */
    if (!found && fallback[0])
    {
        snprintf(out, out_len, "%s", fallback);
        found = 1;
    }

    free(copy);
    return found;
}

static void uri_domain(const char *line, char *out, size_t out_len)
{
    const char *r = strstr(line, "R-URI: ");
    if (!r)
        return;
    r += 7;

    if (strncmp(r, "tel:", 4) == 0 || strncmp(r, "Tel:", 4) == 0)
    {
        snprintf(out, out_len, "tel:");
        return;
    }

    int is_3gpp = 0;
    domain_from_line(r, out, out_len, &is_3gpp);
}

void volte_settings_parse_last_call(const char *slog, volte_call_t *call)
{
    char *copy, *line, *save = NULL;
    int have_call = 0;
    int detail_1223 = 0;
    int reg_subscribe = 0;

    memset(call, 0, sizeof(*call));
    if (!slog)
        return;

    copy = strdup(slog);
    if (!copy)
        return;

    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    {
        /* After each registration ims_rrm subscribes to its own reg event, and that SUBSCRIBE
           logs the same createOutDialog line as a call: skip the one that follows it. */
        if (strstr(line, "SubscribeToRegInfo("))
        {
            reg_subscribe = 1;
        }
        else if (strstr(line, "createOutDialog(): R-URI:") && reg_subscribe)
        {
            reg_subscribe = 0;
        }
        else if (strstr(line, "createOutDialog(): R-URI:"))
        {
            const char *c;

            have_call = 1;
            call->connected = 0;
            call->in_progress = 1;
            call->code = 0;
            call->uri[0] = '\0';
            uri_domain(line, call->uri, sizeof(call->uri));

            /* djb2 of the line, which holds the number: the hash is all that leaves this
               function, so a repeated log yields the same id and a new call a new one. */
            call->id = 5381;
            for (c = line; *c; c++)
                call->id = call->id * 33 + (unsigned char)*c;
        }
        else if (strstr(line, "Service Unavailable (1:223)"))
        {
            detail_1223 = 1;
        }
        /* State 2 is the answered call. 4 is the end, which a refused call reaches straight
           from 0 (Telekom 503: "0 => 4"), so it says nothing about connecting. */
        else if (strstr(line, "onDialogStateChange") && strstr(line, "=> 2"))
        {
            call->connected = 1;
            call->in_progress = 0;
        }
        else if (strstr(line, "onDialogEnded"))
        {
            const char *c = strstr(line, "code ");
            if (c)
                call->code = atoi(c + 5);
            call->in_progress = 0;
        }
    }

    if (!have_call)
        snprintf(call->result, sizeof(call->result), "no call seen");
    else if (call->in_progress)
        snprintf(call->result, sizeof(call->result), "in progress");
    else if (call->connected)
        snprintf(call->result, sizeof(call->result), "connected");
    else if (call->code == 503 && detail_1223)
        snprintf(call->result, sizeof(call->result), "503 (1:223)");
    else if (call->code)
        snprintf(call->result, sizeof(call->result), "failed (%d)", call->code);
    else
        snprintf(call->result, sizeof(call->result), "unknown");

    free(copy);
}

char volte_mode_letter(const char *mode)
{
    if (!mode)
        return 0;
    if (strcmp(mode, "tel") == 0)
        return 't';
    if (strcmp(mode, "sip-imsi") == 0)
        return 'i';
    if (strcmp(mode, "sip-learned") == 0)
        return 'l';
    return 0;
}

const char *volte_mode_from_letter(char letter)
{
    return letter == 't' ? "tel" : letter == 'i' ? "sip-imsi" : letter == 'l' ? "sip-learned" : NULL;
}

const char *volte_auto_next(const char *failed, int learned_differs)
{
    if (!failed)
        failed = "";
    if (!strchr(failed, 't'))
        return "tel";
    if (learned_differs && !strchr(failed, 'l'))
        return "sip-learned";
    if (!strchr(failed, 'i'))
        return "sip-imsi";
    return NULL;
}

int volte_call_refused(const volte_call_t *call)
{
    return !call->connected && call->code == 503;
}

const char *volte_call_mode(const volte_call_t *call, const char *imsi_domain)
{
    if (call->uri[0] == '\0')
        return NULL;
    if (strcmp(call->uri, "tel:") == 0)
        return "tel";
    if (imsi_domain && strcmp(call->uri, imsi_domain) == 0)
        return "sip-imsi";
    return "sip-learned";
}

int volte_settings_parse_apn(const char *xml, const char *service, char *out, size_t out_len)
{
    char name[96];
    const char *p, *end, *apn, *stop;
    int n;
    size_t len;

    if (!out || out_len == 0)
        return 0;
    out[0] = '\0';
    if (!xml || !service)
        return 0;
    n = snprintf(name, sizeof(name), "<Name>%s</Name>", service);
    if (n < 0 || (size_t)n >= sizeof(name) || !(p = strstr(xml, name)))
        return 0;
    p += n;

    /* Only this service's block: a service without an APN must not take the next one's. */
    end = strstr(p, "</ServiceConfiguration>");
    if (!end)
        end = p + strlen(p);
    apn = strstr(p, "<APN>");
    if (!apn || apn >= end)
        return 0;
    apn += 5;
    stop = strstr(apn, "</APN>");
    if (!stop)
        return 0;

    while (apn < stop && isspace((unsigned char)*apn))
        apn++;
    while (stop > apn && isspace((unsigned char)stop[-1]))
        stop--;
    len = (size_t)(stop - apn);
    if (len == 0 || len >= out_len || memchr(apn, '\n', len) || memchr(apn, '<', len))
        return 0;
    memcpy(out, apn, len);
    out[len] = '\0';
    return 1;
}
