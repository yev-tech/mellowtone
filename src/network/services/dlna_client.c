/* Minimal UPnP AV / DLNA ContentDirectory client, see dlna_client.h.
 *
 * The XML here is small and machine-written (device descriptions, SOAP
 * BrowseResponse, DIDL-Lite), so a tolerant scanner is enough: elements are
 * matched by local name whatever their namespace prefix, and text is entity
 * decoded. The DIDL-Lite document arrives entity-escaped inside <Result>, so
 * it is decoded once as a whole and its element text once more. */
#include "dlna_client.h"
#include "branding.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DLNA_MAX_SERVERS 16
#define DLNA_DESC_MAX (256u * 1024u)
#define DLNA_BROWSE_MAX (2u * 1024u * 1024u)

static __thread char last_error[160];

const char * dlna_last_error(void) {
    return last_error;
}

static void set_error(const char * msg) {
    snprintf(last_error, sizeof(last_error), "%s", msg);
}

const char * dlna_codec_name(dlna_codec_t c) {
    switch (c) {
        case DLNA_CODEC_FLAC: return "flac";
        case DLNA_CODEC_MP3: return "mp3";
        case DLNA_CODEC_AAC: return "aac";
        default: return "";
    }
}

/* ---- XML scanning ----------------------------------------------------------- */

/* Decodes the five named entities and numeric references in [s, e) into out
 * (always NUL-terminated). Returns the decoded length. */
static size_t xml_decode(const char * s, const char * e, char * out, size_t size) {
    size_t n = 0;
    if (!size) return 0;
    while (s < e && n + 1 < size) {
        if (*s != '&') { out[n++] = *s++; continue; }
        const char * semi = memchr(s, ';', (size_t) (e - s) < 12 ? (size_t) (e - s) : 12);
        if (!semi) { out[n++] = *s++; continue; }
        size_t len = (size_t) (semi - s - 1);
        const char * name = s + 1;
        unsigned long cp = 0;
        bool ok = true;
        if (len == 2 && !strncmp(name, "lt", 2)) cp = '<';
        else if (len == 2 && !strncmp(name, "gt", 2)) cp = '>';
        else if (len == 3 && !strncmp(name, "amp", 3)) cp = '&';
        else if (len == 4 && !strncmp(name, "quot", 4)) cp = '"';
        else if (len == 4 && !strncmp(name, "apos", 4)) cp = '\'';
        else if (len > 1 && name[0] == '#') cp = (name[1] == 'x' || name[1] == 'X') ? strtoul(name + 2, NULL, 16)
                                                                                   : strtoul(name + 1, NULL, 10);
        else ok = false;
        if (!ok || cp == 0) { out[n++] = *s++; continue; }
        /* UTF-8 encode */
        char buf[4];
        size_t bl;
        if (cp < 0x80) { buf[0] = (char) cp; bl = 1; }
        else if (cp < 0x800) { buf[0] = (char) (0xC0 | (cp >> 6)); buf[1] = (char) (0x80 | (cp & 0x3F)); bl = 2; }
        else if (cp < 0x10000) {
            buf[0] = (char) (0xE0 | (cp >> 12)); buf[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
            buf[2] = (char) (0x80 | (cp & 0x3F)); bl = 3;
        } else {
            buf[0] = (char) (0xF0 | (cp >> 18)); buf[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
            buf[2] = (char) (0x80 | ((cp >> 6) & 0x3F)); buf[3] = (char) (0x80 | (cp & 0x3F)); bl = 4;
        }
        if (n + bl + 1 > size) break;
        memcpy(out + n, buf, bl);
        n += bl;
        s = semi + 1;
    }
    out[n] = '\0';
    return n;
}

typedef struct {
    const char * attrs, * attrs_end; /* inside the start tag, after the name */
    const char * body, * body_end;   /* element content; body == body_end when empty */
    const char * after;              /* just past the element */
} xml_elem_t;

/* Finds the next element named `local` (any namespace prefix) in [p, end). */
static bool xml_next(const char * p, const char * end, const char * local, xml_elem_t * el) {
    size_t ll = strlen(local);
    while (p < end) {
        const char * lt = memchr(p, '<', (size_t) (end - p));
        if (!lt || lt + 1 >= end) return false;
        const char * name = lt + 1;
        if (*name == '/' || *name == '?' || *name == '!') { p = name; continue; }
        const char * ne = name;
        while (ne < end && !isspace((unsigned char) *ne) && *ne != '>' && *ne != '/') ne++;
        const char * colon = memchr(name, ':', (size_t) (ne - name));
        const char * ln = colon ? colon + 1 : name;
        if ((size_t) (ne - ln) != ll || strncmp(ln, local, ll) != 0) { p = ne; continue; }
        const char * gt = memchr(ne, '>', (size_t) (end - ne));
        if (!gt) return false;
        el->attrs = ne;
        el->attrs_end = gt;
        if (gt > ne && gt[-1] == '/') {
            el->attrs_end = gt - 1;
            el->body = el->body_end = gt + 1;
            el->after = gt + 1;
            return true;
        }
        el->body = gt + 1;
        /* matching close tag, same prefix; nesting of the same name is not
         * expected in these documents except <container>/<item>, which never nest */
        char close[80];
        snprintf(close, sizeof(close), "</%.*s>", (int) (ne - name), name);
        size_t cl = strlen(close);
        const char * q = el->body;
        for (;;) {
            const char * c = memchr(q, '<', (size_t) (end - q));
            if (!c || (size_t) (end - c) < cl) return false;
            if (!strncmp(c, close, cl)) { el->body_end = c; el->after = c + cl; return true; }
            q = c + 1;
        }
    }
    return false;
}

/* The decoded text of the first child element `local` in [p, end). */
static bool xml_child_text(const char * p, const char * end, const char * local, char * out, size_t size) {
    xml_elem_t el;
    if (!xml_next(p, end, local, &el)) return false;
    const char * b = el.body, * e = el.body_end;
    if (e - b >= 12 && !strncmp(b, "<![CDATA[", 9)) {
        b += 9;
        const char * ce = strstr(b, "]]>");
        if (ce && ce < e) e = ce;
    }
    while (b < e && isspace((unsigned char) *b)) b++;
    while (e > b && isspace((unsigned char) e[-1])) e--;
    xml_decode(b, e, out, size);
    return true;
}

static bool xml_attr(const char * a, const char * end, const char * name, char * out, size_t size) {
    size_t nl = strlen(name);
    for (const char * p = a; p + nl + 2 <= end; p++) {
        if (strncmp(p, name, nl) != 0 || p[nl] != '=' || (p > a && !isspace((unsigned char) p[-1]))) continue;
        char q = p[nl + 1];
        if (q != '"' && q != '\'') continue;
        const char * v = p + nl + 2;
        const char * ve = memchr(v, q, (size_t) (end - v));
        if (!ve) return false;
        xml_decode(v, ve, out, size);
        return true;
    }
    return false;
}

/* ---- URLs ------------------------------------------------------------------------ */

/* scheme://host[:port] of url into out. */
static void url_origin(const char * url, char * out, size_t size) {
    const char * p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t n = strcspn(p, "/?#");
    snprintf(out, size, "%.*s", (int) (p - url + (ptrdiff_t) n), url);
}

static void url_resolve(const char * base, const char * ref, char * out, size_t size) {
    if (strstr(ref, "://")) { snprintf(out, size, "%s", ref); return; }
    char origin[256];
    url_origin(base, origin, sizeof(origin));
    if (ref[0] == '/') { snprintf(out, size, "%s%s", origin, ref); return; }
    /* relative to the base's directory */
    const char * path = base + strlen(origin);
    const char * slash = strrchr(path, '/');
    int dl = slash ? (int) (slash - path + 1) : 0;
    snprintf(out, size, "%s%.*s%s%s", origin, dl, path, dl ? "" : "/", ref);
}

/* ---- HTTP -------------------------------------------------------------------------- */

static bool http_fetch_ex(const char * url, const char * soap_action, const char * body, size_t max_bytes, char ** out,
                          size_t * out_len, uint32_t connect_ms, http_cancel_token_t * cancel) {
    http_request_t * req = calloc(1, sizeof(*req));
    http_response_t * resp = calloc(1, sizeof(*resp));
    bool ok = false;
    if (!req || !resp) { set_error("Out of memory"); goto done; }
    if (snprintf(req->url, sizeof(req->url), "%s", url) >= (int) sizeof(req->url)) { set_error("Address too long"); goto done; }
    req->method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    req->verify_tls = false; /* LAN servers; DLNA has no TLS in practice */
    req->connect_timeout_ms = connect_ms;
    req->read_timeout_ms = 8000;
    req->total_timeout_ms = 20000;
    req->max_response_bytes = max_bytes;
    req->redirect_limit = 2;
    if (body) {
        req->body = (const uint8_t *) body;
        req->body_len = strlen(body);
        req->content_type = "text/xml; charset=\"utf-8\"";
    }
    if (soap_action) {
        snprintf(req->headers[0].name, sizeof(req->headers[0].name), "SOAPAction");
        snprintf(req->headers[0].value, sizeof(req->headers[0].value), "%s", soap_action);
        req->header_count = 1;
    }
    if (!http_request_ex(req, cancel, resp)) {
        const char * e = resp->error ? resp->error : "";
        if (!strcmp(e, HTTP_ERR_CANCELLED)) set_error("Cancelled");
        else if (!strcmp(e, HTTP_ERR_CONNECT) || !strcmp(e, HTTP_ERR_CONNECT_TIMEOUT)) set_error("Server not reachable");
        else if (!strcmp(e, HTTP_ERR_TIMEOUT)) set_error("Server did not answer");
        else if (!strcmp(e, HTTP_ERR_RESPONSE_TOO_LARGE)) set_error("Answer too large");
        else set_error("Network error");
        goto done;
    }
    if (resp->status != 200 || !resp->body) {
        snprintf(last_error, sizeof(last_error), "Server error %d", resp->status);
        goto done;
    }
    char * b = realloc(resp->body, resp->body_len + 1);
    if (!b) { set_error("Out of memory"); goto done; }
    b[resp->body_len] = '\0';
    *out = b;
    if (out_len) *out_len = resp->body_len;
    resp->body = NULL;
    ok = true;
done:
    if (resp) http_response_free(resp);
    free(req);
    free(resp);
    return ok;
}

static bool http_fetch(const char * url, const char * soap_action, const char * body, size_t max_bytes, char ** out,
                       size_t * out_len, http_cancel_token_t * cancel) {
    return http_fetch_ex(url, soap_action, body, max_bytes, out, out_len, 4000, cancel);
}

/* ---- description ------------------------------------------------------------------- */

static bool parse_description(const char * location, const char * xml, dlna_server_t * out) {
    const char * end = xml + strlen(xml);
    xml_elem_t dev;
    if (!xml_next(xml, end, "device", &dev)) { set_error("Not a UPnP device"); return false; }
    memset(out, 0, sizeof(*out));
    snprintf(out->location, sizeof(out->location), "%s", location);
    /* The root device's own fields come before its serviceList/deviceList. */
    const char * own_end = dev.body_end;
    xml_elem_t sl;
    if (xml_next(dev.body, dev.body_end, "serviceList", &sl)) own_end = sl.body;
    xml_child_text(dev.body, own_end, "friendlyName", out->name, sizeof(out->name));
    xml_child_text(dev.body, own_end, "UDN", out->udn, sizeof(out->udn));
    if (!xml_child_text(dev.body, own_end, "modelName", out->model, sizeof(out->model)) || !out->model[0])
        xml_child_text(dev.body, own_end, "manufacturer", out->model, sizeof(out->model));
    char base[512] = "";
    xml_child_text(xml, end, "URLBase", base, sizeof(base));
    if (!base[0]) snprintf(base, sizeof(base), "%s", location);
    /* ContentDirectory may sit on an embedded device: search the whole document. */
    const char * p = xml;
    xml_elem_t svc;
    while (xml_next(p, end, "service", &svc)) {
        char type[160] = "";
        xml_child_text(svc.body, svc.body_end, "serviceType", type, sizeof(type));
        if (strstr(type, ":service:ContentDirectory:")) {
            char ctl[512] = "";
            xml_child_text(svc.body, svc.body_end, "controlURL", ctl, sizeof(ctl));
            if (ctl[0]) url_resolve(base, ctl, out->control_url, sizeof(out->control_url));
            break;
        }
        p = svc.after;
    }
    if (!out->control_url[0]) { set_error("Not a media server"); return false; }
    if (!out->name[0]) url_origin(location, out->name, sizeof(out->name));
    return true;
}

bool dlna_describe(const char * location, dlna_server_t * out, http_cancel_token_t * cancel) {
    char * xml = NULL;
    if (http_fetch(location, NULL, NULL, DLNA_DESC_MAX, &xml, NULL, cancel)) {
        bool ok = parse_description(location, xml, out);
        free(xml);
        if (ok) return true;
    }
    /* Only an address given: try where common servers keep their description,
     * and with no port given, their usual ports too. */
    const char * after = strstr(location, "://");
    after = after ? after + 3 : location;
    const char * slash = strchr(after, '/');
    if (slash && slash[1]) return false;
    static const struct { int port; const char * path; } known[] = {
        { 50001, "/desc/device.xml" },      /* Synology Media Server */
        { 8200, "/rootDesc.xml" },          /* MiniDLNA / ReadyMedia (most NAS boxes) */
        { 9000, "/dev0/desc.xml" },         /* Twonky */
        { 32469, "/DeviceDescription.xml" }, /* Plex */
    };
    static const char * const paths[] = { "/rootDesc.xml", "/desc/device.xml", "/description.xml", "/dev0/desc.xml",
                                          "/dms/description.xml", "/DeviceDescription.xml", "/dlna/description.xml" };
    char origin[256];
    url_origin(location, origin, sizeof(origin));
    const char * host = strstr(origin, "://");
    host = host ? host + 3 : origin;
    bool has_port = strchr(host, ':') != NULL;
    size_t tries = has_port ? sizeof(paths) / sizeof(paths[0]) : sizeof(known) / sizeof(known[0]);
    for (size_t i = 0; i < tries; i++) {
        char url[512];
        if (has_port) snprintf(url, sizeof(url), "%s%s", origin, paths[i]);
        else snprintf(url, sizeof(url), "%s:%d%s", origin, known[i].port, known[i].path);
        xml = NULL;
        if (!http_fetch_ex(url, NULL, NULL, DLNA_DESC_MAX, &xml, NULL, 1500, cancel)) {
            if (cancel && http_cancel_token_is_cancelled(cancel)) return false;
            continue;
        }
        bool ok = parse_description(url, xml, out);
        free(xml);
        if (ok) return true;
    }
    set_error("No media server at that address");
    return false;
}

/* ---- discovery ----------------------------------------------------------------------- */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

/* Multicast out of the first up, non-loopback IPv4 interface (Wi-Fi here):
 * the player may have no default route. */
static void pick_multicast_if(int fd) {
    struct ifaddrs * ifs = NULL;
    if (getifaddrs(&ifs) != 0) return;
    for (struct ifaddrs * i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        struct in_addr a = ((struct sockaddr_in *) i->ifa_addr)->sin_addr;
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof(a));
        break;
    }
    freeifaddrs(ifs);
}

int dlna_discover(dlna_server_t ** out, int timeout_ms, http_cancel_token_t * cancel) {
    *out = NULL;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { set_error("No network"); return 0; }
    unsigned char ttl = 2;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    pick_multicast_if(fd);
    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET;
    dst.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &dst.sin_addr);
    static const char * const targets[] = { "urn:schemas-upnp-org:device:MediaServer:1",
                                            "urn:schemas-upnp-org:service:ContentDirectory:1" };
    char locations[DLNA_MAX_SERVERS][512];
    int found = 0;
    uint64_t start = now_ms(), next_send = start;
    int sends = 0;
    bool sent_any = false;
    while (now_ms() - start < (uint64_t) timeout_ms) {
        if (cancel && http_cancel_token_is_cancelled(cancel)) break;
        if (sends < 4 && now_ms() >= next_send) {
            char msg[512];
            int len = snprintf(msg, sizeof(msg),
                               "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                               "ST: %s\r\nUSER-AGENT: Linux/1 UPnP/1.0 %s\r\n\r\n",
                               targets[sends % 2], mt_user_agent());
            if (len > 0 && (size_t) len < sizeof(msg) && sendto(fd, msg, (size_t) len, 0, (struct sockaddr *) &dst, sizeof(dst)) == len) sent_any = true;
            sends++;
            next_send = now_ms() + 400;
        }
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        if (poll(&pfd, 1, 100) <= 0) continue;
        char buf[1500];
        ssize_t r = recv(fd, buf, sizeof(buf) - 1, 0);
        if (r <= 0) continue;
        buf[r] = '\0';
        for (char * line = buf; line && *line;) {
            char * nl = strstr(line, "\r\n");
            if (nl) *nl = '\0';
            if (!strncasecmp(line, "LOCATION:", 9)) {
                char * v = line + 9;
                while (*v == ' ' || *v == '\t') v++;
                bool dup = false;
                for (int i = 0; i < found && !dup; i++) dup = !strcmp(locations[i], v);
                if (!dup && found < DLNA_MAX_SERVERS && !strncmp(v, "http://", 7))
                    snprintf(locations[found++], sizeof(locations[0]), "%s", v);
            }
            line = nl ? nl + 2 : NULL;
        }
    }
    close(fd);
    if (!sent_any) { set_error("Could not search the network"); return 0; }
    if (!found) { set_error("No media servers answered"); return 0; }
    dlna_server_t * list = calloc((size_t) found, sizeof(*list));
    if (!list) { set_error("Out of memory"); return 0; }
    int n = 0;
    for (int i = 0; i < found; i++) {
        if (cancel && http_cancel_token_is_cancelled(cancel)) break;
        dlna_server_t s;
        if (!dlna_describe(locations[i], &s, cancel)) continue;
        bool dup = false; /* one device can answer for several targets with different URLs */
        for (int j = 0; j < n && !dup; j++) dup = s.udn[0] && !strcmp(list[j].udn, s.udn);
        if (!dup) list[n++] = s;
    }
    if (!n) { free(list); set_error("No media servers answered"); return 0; }
    *out = list;
    return n;
}

/* ---- browse ------------------------------------------------------------------------------ */

static uint32_t parse_duration(const char * d) {
    /* [H+]:MM:SS[.F+] */
    unsigned h = 0, m = 0;
    double s = 0;
    if (sscanf(d, "%u:%u:%lf", &h, &m, &s) != 3) return 0;
    return (uint32_t) ((h * 3600u + m * 60u) * 1000u + (uint32_t) (s * 1000.0));
}

static dlna_codec_t codec_for_mime(const char * mime) {
    if (!strcasecmp(mime, "audio/flac") || !strcasecmp(mime, "audio/x-flac")) return DLNA_CODEC_FLAC;
    if (!strcasecmp(mime, "audio/mpeg") || !strcasecmp(mime, "audio/mp3") || !strcasecmp(mime, "audio/x-mpeg"))
        return DLNA_CODEC_MP3;
    if (!strcasecmp(mime, "audio/aac") || !strcasecmp(mime, "audio/x-aac") || !strcasecmp(mime, "audio/aacp") ||
        !strcasecmp(mime, "audio/vnd.dlna.adts"))
        return DLNA_CODEC_AAC;
    return DLNA_CODEC_NONE;
}

static int codec_rank(dlna_codec_t c) {
    return c == DLNA_CODEC_FLAC ? 3 : c == DLNA_CODEC_MP3 ? 2 : c == DLNA_CODEC_AAC ? 1 : 0;
}

static void parse_object(const xml_elem_t * el, bool container, dlna_object_t * o) {
    memset(o, 0, sizeof(*o));
    o->is_container = container;
    o->child_count = -1;
    o->track_no = -1;
    xml_attr(el->attrs, el->attrs_end, "id", o->id, sizeof(o->id));
    char cc[16];
    if (xml_attr(el->attrs, el->attrs_end, "childCount", cc, sizeof(cc))) o->child_count = atoi(cc);
    const char * b = el->body, * e = el->body_end;
    xml_child_text(b, e, "title", o->title, sizeof(o->title));
    if (!xml_child_text(b, e, "artist", o->artist, sizeof(o->artist)) || !o->artist[0])
        xml_child_text(b, e, "creator", o->artist, sizeof(o->artist));
    xml_child_text(b, e, "album", o->album, sizeof(o->album));
    xml_child_text(b, e, "albumArtURI", o->art_url, sizeof(o->art_url));
    char cls[96] = "", num[16] = "";
    xml_child_text(b, e, "class", cls, sizeof(cls));
    o->is_album = !strncmp(cls, "object.container.album", 22);
    o->is_audio = !strncmp(cls, "object.item.audioItem", 21);
    if (xml_child_text(b, e, "originalTrackNumber", num, sizeof(num))) o->track_no = atoi(num);
    if (container) return;
    /* Pick the best resource this player can stream: FLAC, then MP3, then AAC. */
    int best = -1;
    const char * p = b;
    xml_elem_t res;
    while (xml_next(p, e, "res", &res)) {
        p = res.after;
        char pi[256] = "";
        xml_attr(res.attrs, res.attrs_end, "protocolInfo", pi, sizeof(pi));
        if (strncmp(pi, "http-get:", 9) != 0) continue;
        char mime[48] = "";
        const char * m = strchr(pi + 9, ':');
        if (m) {
            m++;
            size_t ml = strcspn(m, ":");
            snprintf(mime, sizeof(mime), "%.*s", (int) ml, m);
        }
        if (strncasecmp(mime, "audio/", 6) != 0) continue;
        dlna_codec_t c = codec_for_mime(mime);
        if (codec_rank(c) <= best) continue;
        best = codec_rank(c);
        o->codec = c;
        snprintf(o->mime, sizeof(o->mime), "%s", mime);
        const char * rb = res.body, * re = res.body_end;
        while (rb < re && isspace((unsigned char) *rb)) rb++;
        while (re > rb && isspace((unsigned char) re[-1])) re--;
        xml_decode(rb, re, o->res_url, sizeof(o->res_url));
        char v[32];
        if (xml_attr(res.attrs, res.attrs_end, "duration", v, sizeof(v))) o->duration_ms = parse_duration(v);
        if (xml_attr(res.attrs, res.attrs_end, "sampleFrequency", v, sizeof(v))) o->sample_rate = (unsigned) atoi(v);
        if (xml_attr(res.attrs, res.attrs_end, "bitsPerSample", v, sizeof(v))) o->bits = (unsigned) atoi(v);
        if (xml_attr(res.attrs, res.attrs_end, "nrAudioChannels", v, sizeof(v))) o->channels = (unsigned) atoi(v);
    }
    if (o->codec == DLNA_CODEC_NONE) o->res_url[0] = '\0';
}

bool dlna_browse(const dlna_server_t * server, const char * object_id, int start, int count, dlna_object_t ** out,
                 int * n, int * total, http_cancel_token_t * cancel) {
    *out = NULL;
    *n = 0;
    *total = 0;
    char id[400];
    /* the id goes into XML: escape it */
    size_t k = 0;
    for (const char * p = object_id; *p && k + 7 < sizeof(id); p++) {
        const char * rep = *p == '&' ? "&amp;" : *p == '<' ? "&lt;" : *p == '>' ? "&gt;" : NULL;
        if (rep) { memcpy(id + k, rep, strlen(rep)); k += strlen(rep); }
        else id[k++] = *p;
    }
    id[k] = '\0';
    char body[1200];
    snprintf(body, sizeof(body),
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
             "<u:Browse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
             "<ObjectID>%s</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag><Filter>*</Filter>"
             "<StartingIndex>%d</StartingIndex><RequestedCount>%d</RequestedCount><SortCriteria></SortCriteria>"
             "</u:Browse></s:Body></s:Envelope>",
             id, start, count);
    char * xml = NULL;
    size_t len = 0;
    if (!http_fetch(server->control_url, "\"urn:schemas-upnp-org:service:ContentDirectory:1#Browse\"", body,
                    DLNA_BROWSE_MAX, &xml, &len, cancel))
        return false;
    const char * end = xml + len;
    xml_elem_t res;
    if (!xml_next(xml, end, "Result", &res)) {
        set_error(strstr(xml, "Fault") ? "The server refused to list this folder" : "Unexpected answer");
        free(xml);
        return false;
    }
    char tm[16] = "";
    if (xml_child_text(res.after, end, "TotalMatches", tm, sizeof(tm))) *total = atoi(tm);
    size_t rl = (size_t) (res.body_end - res.body);
    char * didl = malloc(rl + 1);
    if (!didl) { free(xml); set_error("Out of memory"); return false; }
    const char * rb = res.body, * re = res.body_end;
    if (re - rb >= 12 && !strncmp(rb, "<![CDATA[", 9)) {
        rb += 9;
        const char * ce = strstr(rb, "]]>");
        if (ce && ce < re) re = ce;
        memcpy(didl, rb, (size_t) (re - rb));
        didl[re - rb] = '\0';
        rl = (size_t) (re - rb);
    } else {
        rl = xml_decode(rb, re, didl, rl + 1);
    }
    free(xml);
    /* Count objects, then parse in document order. */
    const char * dend = didl + rl;
    int cap = 0;
    for (const char * p = didl; (p = strstr(p, "<container")) != NULL; p++) cap++;
    for (const char * p = didl; (p = strstr(p, "<item")) != NULL; p++) cap++;
    dlna_object_t * list = cap ? calloc((size_t) cap, sizeof(*list)) : NULL;
    if (cap && !list) { free(didl); set_error("Out of memory"); return false; }
    int got = 0;
    const char * p = didl;
    while (p < dend && got < cap) {
        xml_elem_t c, i;
        bool hc = xml_next(p, dend, "container", &c);
        bool hi = xml_next(p, dend, "item", &i);
        if (!hc && !hi) break;
        bool use_c = hc && (!hi || c.attrs < i.attrs);
        parse_object(use_c ? &c : &i, use_c, &list[got]);
        p = use_c ? c.after : i.after;
        if (list[got].id[0]) got++;
    }
    free(didl);
    if (!got) { free(list); list = NULL; }
    *out = list;
    *n = got;
    if (*total < got + start) *total = got + start;
    return true;
}
