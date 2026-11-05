#include "carrier_fetch.h"
#include "ims_carrier_db.h"

#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#define TEXTPB_URL "https://android.googlesource.com/platform/packages/providers/TelephonyProvider/+/main/assets/latest_carrier_id/carrier_list.textpb?format=TEXT"
#define APNS_URL   "https://android.googlesource.com/device/sample/+/refs/heads/main/etc/apns-full-conf.xml?format=TEXT"
#define CA_PATH    "/etc/openssl/certs"

#define MAX_DOWNLOAD (8 * 1024 * 1024)

typedef struct
{
    char *data;
    size_t len;
} download_t;

static size_t download_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    download_t *d = userdata;
    size_t bytes = size * nmemb;
    if (d->len + bytes > MAX_DOWNLOAD)
        return 0;

    char *data = realloc(d->data, d->len + bytes + 1);
    if (!data)
        return 0;

    memcpy(data + d->len, ptr, bytes);
    d->data = data;
    d->len += bytes;
    d->data[d->len] = '\0';
    return bytes;
}

/* Decodes in place, since gitiles serves ?format=TEXT as base64. Returns the decoded length. */
static size_t base64_decode(char *s)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned int bits = 0;
    int have = 0;
    size_t out = 0;

    for (const char *p = s; *p && *p != '='; p++)
    {
        const char *pos = strchr(table, *p);
        if (!pos)
            continue;

        bits = (bits << 6) | (unsigned int)(pos - table);
        have += 6;
        if (have >= 8)
        {
            have -= 8;
            s[out++] = (char)((bits >> have) & 0xff);
        }
    }
    s[out] = '\0';
    return out;
}

/*
 * The peer stays verified: this data decides which APN the phone uses and is read by root.
 * TLS 1.2 is what the phone's OpenSSL 1.0.2 negotiates by default; libcurl 7.24 has no option
 * to request it by name.
 * ponytail: relies on the OS GlobalSign root, which expires in 2028; ship a CA file in data/ if
 * the phone can no longer verify the chain by then.
 */
static int download(CURL *curl, const char *url, download_t *d)
{
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, d);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAPATH, CA_PATH);
    curl_easy_setopt(curl, CURLOPT_CAINFO, NULL);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "bb10-volte");

    return curl_easy_perform(curl) == CURLE_OK && d->data ? 0 : -1;
}

int ims_carrier_db_fetch(const char *stock_path, const char *out_db, const char *out_apns)
{
    download_t textpb = {0}, apns = {0};
    int status = -1;

    curl_global_init(CURL_GLOBAL_ALL);
    CURL *curl = curl_easy_init();

    if (curl && download(curl, TEXTPB_URL, &textpb) == 0 && download(curl, APNS_URL, &apns) == 0)
    {
        base64_decode(textpb.data);
        size_t apns_len = base64_decode(apns.data);
        status = ims_carrier_db_generate(textpb.data, apns.data, apns_len, stock_path, out_db, out_apns);
    }

    if (curl)
        curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(textpb.data);
    free(apns.data);
    return status;
}
