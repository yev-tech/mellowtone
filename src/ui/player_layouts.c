/* The XML loader behind the Player screen's layout. See player_layouts.h
 * for the model and docs/PLAYER_LAYOUTS.md for the widget-name contract. */
#include "player_layouts.h"
#include "board_config.h"
#include "fallback_font.h"
#include "assets.h"

#include "../../vendor/lv_xml/src/others/xml/lv_xml.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_XML_BYTES (256 * 1024)
/* The one XML component name in use. A rebuild unregisters the previous
 * component before it registers the layout again. */
#define XML_COMPONENT_NAME "mellowtone_player_layout"

static bool xml_engine_ready;
static bool xml_globals_ready;
static bool xml_component_registered;

/* The widget images the layout names, as file stems of assets resolved
 * through asset_path() so theme overrides apply. The C builder references the
 * same files, so LVGL's image cache holds each of them once. */
typedef struct {
    const char * name;
    const char * relative_path;
} layout_image_t;

static const layout_image_t layout_images[] = {
    { "default_cover", "playing_plane/default_cover_565.png" },
    { "collect_out", "playing_plane/collect_out.png" },
    { "btn_prev", "playing_plane/btn_prev.png" },
    { "btn_next", "playing_plane/btn_next.png" },
    { "btn_play", "playing_plane/btn_play.png" },
    { "ic_more", "playing_plane/ic_more.png" },
    { "order", "playing_plane/order.png" },
};

/* ---- XML creation ---- */

static char * read_text_file(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    char * buffer = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > 0 && size <= MAX_XML_BYTES && fseek(f, 0, SEEK_SET) == 0) {
            buffer = malloc((size_t) size + 1);
            if (buffer && fread(buffer, 1, (size_t) size, f) == (size_t) size) {
                buffer[size] = '\0';
            } else {
                free(buffer);
                buffer = NULL;
            }
        }
    }
    fclose(f);
    return buffer;
}

/* A layout that carries the comment DESIGN_MARKER was drawn on the 360x480
 * design panel of the modern UI (BOARD_UI_PX(), board_config.h). On any
 * other panel its pixel geometry is rescaled before the engine sees it: horizontal values and
 * sizes (x, width, height, radius, padding, margins, <px> constants) by
 * BOARD_UI_PX(), vertical positions (y, translate_y) by BOARD_UI_PY(). Only
 * plain integers are touched; "#const", "50%" and the like stay as they
 * are. Returns a new buffer, or NULL when nothing needs to change. */
#define DESIGN_MARKER "mellowtone:design 360x480"

static bool xml_attr_is(const char * name, size_t len, const char * want) {
    return strlen(want) == len && strncmp(name, want, len) == 0;
}

static bool xml_attr_has_prefix(const char * name, size_t len, const char * prefix) {
    size_t n = strlen(prefix);
    return len > n && strncmp(name, prefix, n) == 0;
}

/* 0: leave, 1: horizontal/size, 2: vertical position. */
static int xml_attr_axis(const char * tag, size_t tag_len, const char * name, size_t len) {
    if (xml_attr_is(name, len, "y") || xml_attr_is(name, len, "style_translate_y")) return 2;
    if (xml_attr_is(name, len, "x") || xml_attr_is(name, len, "width") || xml_attr_is(name, len, "height") ||
        xml_attr_is(name, len, "style_width") || xml_attr_is(name, len, "style_height") ||
        xml_attr_is(name, len, "style_radius") || xml_attr_is(name, len, "style_translate_x") ||
        xml_attr_has_prefix(name, len, "style_pad_") || xml_attr_has_prefix(name, len, "style_margin_"))
        return 1;
    if (xml_attr_is(name, len, "value") && xml_attr_is(tag, tag_len, "px")) return 1;
    return 0;
}

static char * scale_design_layout(const char * xml) {
    if (BOARD_SCREEN_WIDTH == BOARD_UI_DESIGN_WIDTH && BOARD_SCREEN_HEIGHT == BOARD_UI_DESIGN_HEIGHT) return NULL;
    if (!strstr(xml, DESIGN_MARKER)) return NULL;
    size_t len = strlen(xml);
    /* Every rewritten value grows by at most a few digits. */
    size_t cap = len * 2 + 64;
    char * out = malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    const char * p = xml;
    const char * tag = NULL;
    size_t tag_len = 0;
    while (*p && o + 16 < cap) {
        if (strncmp(p, "<!--", 4) == 0) { /* comments are copied untouched */
            const char * end = strstr(p, "-->");
            size_t n = end ? (size_t) (end + 3 - p) : strlen(p);
            if (o + n >= cap) break;
            memcpy(out + o, p, n);
            o += n;
            p += n;
            continue;
        }
        if (*p == '<') {
            const char * q = p + 1;
            while (*q == '/') q++;
            tag = q;
            while (isalnum((unsigned char) *q) || *q == '_') q++;
            tag_len = (size_t) (q - tag);
        }
        /* name="123" inside a tag */
        if ((isalpha((unsigned char) *p) || *p == '_') && tag && (p == xml || isspace((unsigned char) p[-1]))) {
            const char * name = p;
            const char * q = p;
            while (isalnum((unsigned char) *q) || *q == '_') q++;
            size_t name_len = (size_t) (q - name);
            int axis = q[0] == '=' && q[1] == '"' ? xml_attr_axis(tag, tag_len, name, name_len) : 0;
            if (axis) {
                const char * v = q + 2;
                char * end = NULL;
                long value = strtol(v, &end, 10);
                if (end != v && *end == '"' && (isdigit((unsigned char) *v) || *v == '-')) {
                    int32_t px = (int32_t) value;
                    int32_t scaled = axis == 2 ? BOARD_UI_PY(px) : BOARD_UI_PX(px);
                    int n = snprintf(out + o, cap - o, "%.*s=\"%ld\"", (int) name_len, name, (long) scaled);
                    if (n < 0 || (size_t) n >= cap - o) break;
                    o += (size_t) n;
                    p = end + 1;
                    continue;
                }
            }
            memcpy(out + o, name, name_len);
            o += name_len;
            p = q;
            continue;
        }
        if (*p == '>') tag = NULL;
        out[o++] = *p++;
    }
    if (*p) { /* ran out of room: keep the layout as written */
        free(out);
        return NULL;
    }
    out[o] = '\0';
    return out;
}

/* Fonts and constants are global in the XML engine and first registration
 * wins, so they are registered once. The font handles are the app's stable
 * lv_font_t objects, whose contents are replaced in place on a font change,
 * and the screen size is fixed for the life of the process. */
static void register_xml_globals_once(void) {
    if (xml_globals_ready) return;
    lv_xml_register_font(NULL, "player_meta", &app_font_player_meta);
    lv_xml_register_font(NULL, "font_16", &app_font_16);
    lv_xml_register_font(NULL, "font_22", &app_font_22);
    char value[16];
    snprintf(value, sizeof(value), "%d", BOARD_SCREEN_WIDTH);
    lv_xml_register_const(NULL, "screen_w", value);
    snprintf(value, sizeof(value), "%d", BOARD_SCREEN_HEIGHT);
    lv_xml_register_const(NULL, "screen_h", value);
    xml_globals_ready = true;
}

void player_layouts_release(void) {
    if (!xml_component_registered) return;
    lv_xml_component_unregister(XML_COMPONENT_NAME);
    xml_component_registered = false;
}

static void delete_children_from(lv_obj_t * parent, uint32_t first) {
    while (lv_obj_get_child_count(parent) > first) lv_obj_delete(lv_obj_get_child(parent, (int32_t) first));
}

/* The engine registers its widget parsers and a global scope on start, so it
 * is started by the first XML layout rather than for everyone. It lives for
 * the rest of the process: soft UI reloads keep LVGL, and with it the engine,
 * alive, and each layout's own component is released separately. */
static void start_xml_engine_once(void) {
    if (xml_engine_ready) return;
    lv_xml_init();
    xml_engine_ready = true;
}

lv_obj_t * player_layouts_create(lv_obj_t * parent) {
    if (!parent) return NULL;
    const char * path = asset_path_plain(PLAYER_LAYOUT_FILE);
    if (!path) return NULL;
    char * xml = read_text_file(path);
    if (!xml) {
        fprintf(stderr, "Warning: player layout: cannot read %s\n", path);
        return NULL;
    }
    char * scaled = scale_design_layout(xml);
    if (scaled) {
        free(xml);
        xml = scaled;
    }

    start_xml_engine_once();
    register_xml_globals_once();
    player_layouts_release();
    lv_result_t res = lv_xml_register_component_from_data(XML_COMPONENT_NAME, xml);
    free(xml);
    if (res != LV_RESULT_OK) {
        fprintf(stderr, "Warning: player layout: %s is not a valid LVGL XML component\n", path);
        return NULL;
    }
    xml_component_registered = true;

    /* Images go into the component's own scope, after its metadata pass: the
     * paths depend on theme overrides, which may differ between builds, and
     * the scope is released together with the layout. A name the XML declares
     * itself keeps precedence. */
    lv_xml_component_scope_t * scope = lv_xml_component_get_scope(XML_COMPONENT_NAME);
    for (size_t i = 0; scope && i < sizeof(layout_images) / sizeof(layout_images[0]); ++i)
        lv_xml_register_image(scope, layout_images[i].name, asset_path(layout_images[i].relative_path));

    uint32_t children_before = lv_obj_get_child_count(parent);
    lv_obj_t * root = lv_xml_create(parent, XML_COMPONENT_NAME, NULL);
    if (!root) {
        fprintf(stderr, "Warning: player layout: %s failed to build\n", path);
        delete_children_from(parent, children_before);
        player_layouts_release();
        return NULL;
    }
    return root;
}
