#include "carrier_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

/* -------------------------------------------------------------------------
 * VoLTE profile builder
 * ------------------------------------------------------------------------- */

#define RIM_CARRIER_NS "http://www.rim.com/CarrierConfig/2012-01"

/* Both services come from the CoopVoce profile that has passed real VoLTE calls. */
static const char k_service_templates[] =
    "<templates xmlns=\"" RIM_CARRIER_NS "\">"
    "<ServiceConfiguration>"
    "<Name>mms</Name>"
    "<Description>Configuration for mms data</Description>"
    "<NetworkInterfaceConfiguration><IPConfiguration><Type>IPv4</Type></IPConfiguration></NetworkInterfaceConfiguration>"
    "<BearerManagementTimer/>"
    "<RadioAccessTechnology><APN></APN></RadioAccessTechnology>"
    "</ServiceConfiguration>"
    "<ServiceConfiguration>"
    "<Name>ims</Name>"
    "<Description>Configuration for IMS data for VoLTE-capable devices</Description>"
    "<Connectivity>"
    "<NotRoaming>AllowIfDataServicesEnabledOrVoLTECallActive</NotRoaming>"
    "<Roaming>AllowIfDataRoamingEnabledOrVoLTECallActive</Roaming>"
    "</Connectivity>"
    "<AlwaysOn>false</AlwaysOn>"
    "<NetworkInterfaceConfiguration><IPConfiguration>"
    "<Type>IPv4v6</Type><ProtectRouting>true</ProtectRouting><DisableDAD>true</DisableDAD>"
    "</IPConfiguration></NetworkInterfaceConfiguration>"
    "<BearerManagementTimer><KeepAlive>0</KeepAlive></BearerManagementTimer>"
    "<RadioAccessTechnology><APN>ims</APN></RadioAccessTechnology>"
    "<IMSConfiguration>"
    "<NetworkServiceFilter>3GPP</NetworkServiceFilter>"
    "<RequestPCSCFViaPCO>true</RequestPCSCFViaPCO>"
    "<RequestSignallingBearerViaPCO>true</RequestSignallingBearerViaPCO>"
    "</IMSConfiguration>"
    "<IMSConfiguration><RequestPCSCFViaPCO>true</RequestPCSCFViaPCO></IMSConfiguration>"
    "</ServiceConfiguration>"
    "</templates>";

typedef struct
{
    char name[64];
    char apn[256];
    char user[128];
    char password[128];
    char mms_apn[256];
    char mms_user[128];
    char mms_password[128];
} carrier_apns_t;

static xmlNodePtr find_child(xmlNodePtr parent, const char *name)
{
    for (xmlNodePtr n = parent->children; n; n = n->next)
    {
        if (n->type == XML_ELEMENT_NODE && xmlStrcmp(n->name, (const xmlChar *)name) == 0)
            return n;
    }
    return NULL;
}

/* The element of this kind under parent whose <Name> is name. */
static xmlNodePtr find_named(xmlNodePtr parent, const char *kind, const char *name)
{
    for (xmlNodePtr n = parent->children; n; n = n->next)
    {
        if (n->type != XML_ELEMENT_NODE || xmlStrcmp(n->name, (const xmlChar *)kind) != 0)
            continue;

        xmlNodePtr name_node = find_child(n, "Name");
        xmlChar *text = name_node ? xmlNodeGetContent(name_node) : NULL;
        int match = text && xmlStrcmp(text, (const xmlChar *)name) == 0;
        xmlFree(text);
        if (match)
            return n;
    }
    return NULL;
}

static void set_text(xmlDocPtr doc, xmlNodePtr node, const char *value)
{
    xmlChar *escaped = xmlEncodeSpecialChars(doc, (const xmlChar *)value);
    xmlNodeSetContent(node, escaped);
    xmlFree(escaped);
}

static void use_namespace(xmlNodePtr node, xmlNsPtr ns)
{
    for (; node; node = node->next)
    {
        if (node->type == XML_ELEMENT_NODE)
        {
            node->ns = ns;
            use_namespace(node->children, ns);
        }
    }
}

/* Replaces the name's alias, then places the service with the others, ahead of the aliases. */
static void add_service(xmlDocPtr doc, xmlNodePtr data, xmlDocPtr templates, const char *name)
{
    xmlNodePtr alias = find_named(data, "ServiceConfigurationAlias", name);
    if (alias)
    {
        xmlUnlinkNode(alias);
        xmlFreeNode(alias);
    }

    xmlNodePtr source = find_named(xmlDocGetRootElement(templates), "ServiceConfiguration", name);
    xmlNodePtr copy = source ? xmlDocCopyNode(source, doc, 1) : NULL;
    if (!copy)
        return;

    /* The copy declares its own namespace; profiles the data service has read declare one. */
    xmlFreeNsList(copy->nsDef);
    copy->nsDef = NULL;
    copy->ns = data->ns;
    use_namespace(copy->children, data->ns);

    xmlNodePtr first_alias = find_child(data, "ServiceConfigurationAlias");
    if (first_alias)
        xmlAddPrevSibling(first_alias, copy);
    else
        xmlAddChild(data, copy);
}

static void remove_child(xmlNodePtr parent, const char *name)
{
    xmlNodePtr child = find_child(parent, name);
    if (child)
    {
        xmlUnlinkNode(child);
        xmlFreeNode(child);
    }
}

static void set_service_apn(xmlDocPtr doc, xmlNodePtr service, const char *apn, const char *user, const char *password)
{
    xmlNodePtr rat = service ? find_child(service, "RadioAccessTechnology") : NULL;
    xmlNodePtr apn_node = rat ? find_child(rat, "APN") : NULL;
    if (!apn_node)
        return;

    /* A stock login or proxy belongs to the stock APN and would be wrong for a different one. */
    xmlChar *old_apn = xmlNodeGetContent(apn_node);
    int apn_changed = !old_apn || xmlStrcmp(old_apn, (const xmlChar *)apn) != 0;
    int has_login = user[0] != '\0' || password[0] != '\0';
    xmlFree(old_apn);

    if (apn_changed || has_login)
        remove_child(rat, "Authentication");
    if (apn_changed)
    {
        remove_child(rat, "HttpProxyV4");
        remove_child(service, "HttpProxyV4");
    }

    set_text(doc, apn_node, apn);

    if (has_login)
    {
        /* PAP is the only method any stock config uses. */
        xmlNodePtr auth = xmlNewChild(rat, apn_node->ns, (const xmlChar *)"Authentication", NULL);
        xmlNodePtr pap = xmlNewChild(auth, apn_node->ns, (const xmlChar *)"PAP", NULL);
        set_text(doc, xmlNewChild(pap, apn_node->ns, (const xmlChar *)"Username", NULL), user);
        set_text(doc, xmlNewChild(pap, apn_node->ns, (const xmlChar *)"Password", NULL), password);
    }
}

/*
 * list is the Android pick, then any other APNs Android has for the carrier. A stock APN among them is
 * BlackBerry's choice of the same plans, so it stays with its login; otherwise the pick replaces it.
 */
static void set_listed_apn(xmlDocPtr doc, xmlNodePtr service, const char *list, const char *user, const char *password)
{
    xmlNodePtr rat = service ? find_child(service, "RadioAccessTechnology") : NULL;
    xmlNodePtr apn_node = rat ? find_child(rat, "APN") : NULL;
    xmlChar *stock = apn_node ? xmlNodeGetContent(apn_node) : NULL;
    size_t stock_len = stock ? strlen((const char *)stock) : 0;
    char first[128];
    snprintf(first, sizeof(first), "%.*s", (int)strcspn(list, ","), list);

    /* Stock equal to the pick still takes the pick, with Android's login and spelling. */
    int keep = 0;
    if (stock_len && strcasecmp(first, (const char *)stock) != 0)
    {
        for (const char *p = list + strcspn(list, ","); *p && !keep; p += strcspn(p, ","))
        {
            p++;
            keep = strcspn(p, ",") == stock_len && strncasecmp(p, (const char *)stock, stock_len) == 0;
        }
    }
    xmlFree(stock);

    if (!keep)
        set_service_apn(doc, service, first, user, password);
}

/* Adds a setting where stock configs keep it: after the first of these that the service has. */
static void add_setting(xmlNodePtr service, const char *name, const char *after_1, const char *after_2)
{
    xmlNodePtr after = find_child(service, after_1);
    after = after ? after : find_child(service, after_2);
    xmlNodePtr setting = xmlNewNode(service->ns, (const xmlChar *)name);
    xmlNodeSetContent(setting, (const xmlChar *)"true");
    if (after)
        xmlAddNextSibling(after, setting);
    else
        xmlAddChild(service, setting);
}

/* The data service refuses APN changes on a service marked false, and 25 stock configs do that. */
static void make_internet_editable(xmlDocPtr doc, xmlNodePtr data)
{
    xmlNodePtr internet = find_named(data, "ServiceConfiguration", "internet");
    if (!internet)
        return;

    xmlNodePtr modifiable = find_child(internet, "AppModifiable");
    if (modifiable)
        set_text(doc, modifiable, "true");
    else
        add_setting(internet, "AppModifiable", "AppSelectable", "AlwaysOn");
}

/*
 * Most stock "ims" services are plain data bearers with no IMS settings, so only one with an
 * IMSConfiguration is kept. A replaced one leaves two things behind: a carrier's own APN, and the
 * LTE attach role, because a profile with no attach service can strand the phone off LTE.
 */
static void ensure_ims_service(xmlDocPtr doc, xmlNodePtr data, xmlDocPtr templates)
{
    xmlNodePtr old = find_named(data, "ServiceConfiguration", "ims");
    xmlChar *old_apn = NULL;
    int had_attach = 0;

    if (old)
    {
        if (find_child(old, "IMSConfiguration"))
            return;

        xmlNodePtr rat = find_child(old, "RadioAccessTechnology");
        xmlNodePtr apn = rat ? find_child(rat, "APN") : NULL;
        old_apn = apn ? xmlNodeGetContent(apn) : NULL;
        had_attach = find_child(old, "LTEAttachService") != NULL;
        xmlUnlinkNode(old);
        xmlFreeNode(old);
    }

    add_service(doc, data, templates, "ims");

    if (old_apn && old_apn[0] != '\0')
    {
        xmlNodePtr service = find_named(data, "ServiceConfiguration", "ims");
        xmlNodePtr rat = service ? find_child(service, "RadioAccessTechnology") : NULL;
        xmlNodePtr apn = rat ? find_child(rat, "APN") : NULL;
        if (apn)
            set_text(doc, apn, (const char *)old_apn);
    }
    xmlFree(old_apn);

    xmlNodePtr internet = find_named(data, "ServiceConfiguration", "internet");
    if (had_attach && internet && !find_child(internet, "LTEAttachService"))
        add_setting(internet, "LTEAttachService", "AlwaysOn", "Description");
}

static char *next_tsv_field(char *field, char *out, size_t size)
{
    if (!field)
    {
        out[0] = '\0';
        return NULL;
    }

    char *next = strchr(field, '\t');
    if (next)
        *next++ = '\0';
    snprintf(out, size, "%s", field);
    return next;
}

static int load_carrier_apns(const char *path, int carrier_id, carrier_apns_t *out)
{
    char line[1024];
    int found = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    while (!found && fgets(line, sizeof(line), f))
    {
        if (atoi(line) != carrier_id)
            continue;

        line[strcspn(line, "\r\n")] = '\0';
        char *field = strchr(line, '\t');
        field = field ? field + 1 : NULL;
        field = next_tsv_field(field, out->name, sizeof(out->name));
        field = next_tsv_field(field, out->apn, sizeof(out->apn));
        field = next_tsv_field(field, out->user, sizeof(out->user));
        field = next_tsv_field(field, out->password, sizeof(out->password));
        field = next_tsv_field(field, out->mms_apn, sizeof(out->mms_apn));
        field = next_tsv_field(field, out->mms_user, sizeof(out->mms_user));
        next_tsv_field(field, out->mms_password, sizeof(out->mms_password));
        found = 1;
    }

    fclose(f);
    return found;
}

static void apply_carrier_apns(xmlDocPtr doc, xmlNodePtr data, xmlDocPtr templates, const carrier_apns_t *apns)
{
    xmlNodePtr name = find_child(data, "Name");
    if (name && apns->name[0] != '\0')
        set_text(doc, name, apns->name);

    if (apns->apn[0] != '\0')
        set_listed_apn(doc, find_named(data, "ServiceConfiguration", "internet"), apns->apn, apns->user, apns->password);

    if (apns->mms_apn[0] != '\0')
    {
        if (!find_named(data, "ServiceConfiguration", "mms"))
            add_service(doc, data, templates, "mms");
        set_listed_apn(doc, find_named(data, "ServiceConfiguration", "mms"), apns->mms_apn, apns->mms_user, apns->mms_password);
    }
}

static int same_as_file(const char *path, const xmlChar *mem, int len)
{
    int same = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;

    char *old = malloc((size_t)len + 1);
    if (old)
    {
        /* Reading one byte extra tells a longer file apart from an identical start. */
        same = fread(old, 1, (size_t)len + 1, f) == (size_t)len && memcmp(old, mem, (size_t)len) == 0;
        free(old);
    }
    fclose(f);
    return same;
}

static int save_profile(xmlDocPtr doc, const char *path)
{
    xmlChar *mem = NULL;
    int len = 0;
    int status = -1;
    char tmp[600];

    xmlDocDumpFormatMemoryEnc(doc, &mem, &len, "UTF-8", 1);
    if (!mem)
        return -1;

    if (same_as_file(path, mem, len))
    {
        xmlFree(mem);
        return 0;
    }

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f)
    {
        size_t written = fwrite(mem, 1, (size_t)len, f);
        if (fclose(f) == 0 && written == (size_t)len && chmod(tmp, 0644) == 0 && rename(tmp, path) == 0)
            status = 0;
        else
            remove(tmp);
    }

    xmlFree(mem);
    return status;
}

int ims_build_carrier_xml(int carrier_id, const char *config_dir, const char *apns_path, const char *out_path)
{
    char source[600];
    carrier_apns_t apns;
    memset(&apns, 0, sizeof(apns));
    int has_apns = load_carrier_apns(apns_path, carrier_id, &apns);

    /* Once linked, <id>.xml is this profile. Rebuilding it from itself changes nothing. */
    snprintf(source, sizeof(source), "%s/%d.xml", config_dir, carrier_id);
    if (access(source, R_OK) != 0)
        snprintf(source, sizeof(source), "%s/0.xml", config_dir);

    xmlInitParser();

    int status = -1;
    xmlDocPtr doc = xmlReadFile(source, NULL, XML_PARSE_NONET | XML_PARSE_NOBLANKS);
    xmlDocPtr templates = xmlReadMemory(k_service_templates, (int)sizeof(k_service_templates) - 1, "templates.xml",
                                        NULL, XML_PARSE_NONET | XML_PARSE_NOBLANKS);
    xmlNodePtr root = doc ? xmlDocGetRootElement(doc) : NULL;
    xmlNodePtr data = root ? find_child(root, "CarrierDataConfiguration") : NULL;

    if (data && templates)
    {
        if (has_apns)
            apply_carrier_apns(doc, data, templates, &apns);

        ensure_ims_service(doc, data, templates);
        make_internet_editable(doc, data);

        status = save_profile(doc, out_path);
    }

    if (templates)
        xmlFreeDoc(templates);
    if (doc)
        xmlFreeDoc(doc);
    return status;
}
