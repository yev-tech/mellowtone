#include "branding.h"

#include "app_version.h"

#include <pthread.h>
#include <stdio.h>

#define MT_UA_VERSION_MAX 48

static char user_agent[160];
static pthread_once_t user_agent_once = PTHREAD_ONCE_INIT;

static int is_token_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '-' || c == '_' || c == '+';
}

static void build_user_agent(void) {
    const char * label = app_version_label();
    char version[MT_UA_VERSION_MAX + 1];
    size_t n = 0;
    for (const unsigned char * p = (const unsigned char *) (label ? label : ""); *p && n < MT_UA_VERSION_MAX; p++) {
        char c = is_token_char(*p) ? (char) *p : '-';
        if (c == '-' && (n == 0 || version[n - 1] == '-')) continue;
        version[n++] = c;
    }
    while (n > 0 && version[n - 1] == '-') n--;
    if (n == 0) version[n++] = '0';
    version[n] = '\0';

    if (MT_CONTACT[0])
        snprintf(user_agent, sizeof(user_agent), "%s/%s ( %s )", MT_PRODUCT_NAME, version, MT_CONTACT);
    else
        snprintf(user_agent, sizeof(user_agent), "%s/%s", MT_PRODUCT_NAME, version);
}

const char * mt_user_agent(void) {
    pthread_once(&user_agent_once, build_user_agent);
    return user_agent;
}
