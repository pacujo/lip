#include <fsdyn/charstr.h>
#include <nwutil.h>

static bool exhausted(const char *p, const char *end)
{
    return !p || p == end || !*p;
}

static const char *skip_ascii(char c, const char *p, const char *end)
{
    if (exhausted(p, end) || *p != c)
        return NULL;
    return p + 1;
}

static const char *skip_string(const char *s, const char *p, const char *end)
{
    for (; *s; s++)
        p = skip_ascii(*s, p, end);
    return p;
}

static bool at_final_jam(const char *p, const char *end)
{
    while (!exhausted(p, end)) {
        int codepoint;
        p = charstr_decode_utf8_codepoint(p, end, &codepoint);
        switch (codepoint) {
            case ' ':
            case '<':
            case '>':
                return true;
            case ')':
            case '.':
            case ',':
            case ':':
            case ';':
            case '!':
            case '?':
            case '"':
            case '\'':
                continue;
            default:
                if (codepoint >= 128)
                    switch (charstr_unicode_category(codepoint)) {
                        case UNICODE_CATEGORY_Cs:
                        case UNICODE_CATEGORY_Pc:
                        case UNICODE_CATEGORY_Pd:
                        case UNICODE_CATEGORY_Pe:
                        case UNICODE_CATEGORY_Pf:
                        case UNICODE_CATEGORY_Pi:
                        case UNICODE_CATEGORY_Po:
                        case UNICODE_CATEGORY_Ps:
                        case UNICODE_CATEGORY_Zl:
                        case UNICODE_CATEGORY_Zp:
                        case UNICODE_CATEGORY_Zs:
                            continue;
                        default:
                            return false;
                    }
                return (charstr_char_class(codepoint) & CHARSTR_CONTROL) != 0;
        }
    }
    return true;
}

/* Heuristically find the end of a URL. Return NULL if no URL is
 * detected at start. */
static const char *skip_url(const char *start, const char *end)
{
    static const char *const schemes[] = {
        "http",
        "https",
        NULL
    };
    for (const char *const *sp = schemes; *sp; sp++) {
        const char *p = skip_string(*sp, start, end);
        p = skip_ascii(':', p, end);
        p = skip_ascii('/', p, end);
        p = skip_ascii('/', p, end);
        if (p) {
            while (!at_final_jam(p, end))
                p = charstr_skip_utf8_grapheme(p, end);
            return p;
        }
    }
    return NULL;
}

/* Heuristically find the beginning and end of a URL. Return NULL if
 * no URL is detected between p and end. */
const char *find_url(const char *p, const char *end, const char **url_end)
{
    for (;;) {
        *url_end = skip_url(p, end);
        if (*url_end) {
            /* The end of the URL has been found. Let's see if the URL
             * is compliant: */
            nwutil_url_t *url = nwutil_parse_url(p, *url_end - p, NULL);
            if (url) {
                nwutil_url_destroy(url);
                return p;
            }
        }
        if (exhausted(p, end))
            return NULL;
        for (;;) {
            int codepoint;
            p = charstr_decode_utf8_codepoint(p, end, &codepoint);
            if (!p)
                return NULL;
            switch (charstr_unicode_category(codepoint)) {
                case UNICODE_CATEGORY_Ll:
                case UNICODE_CATEGORY_Lm:
                case UNICODE_CATEGORY_Lo:
                case UNICODE_CATEGORY_Lt:
                case UNICODE_CATEGORY_Lu:
                case UNICODE_CATEGORY_Mc:
                case UNICODE_CATEGORY_Me:
                case UNICODE_CATEGORY_Mn:
                case UNICODE_CATEGORY_Nd:
                case UNICODE_CATEGORY_Nl:
                case UNICODE_CATEGORY_No:
                case UNICODE_CATEGORY_Pc:
                case UNICODE_CATEGORY_Pd:
                    continue;
                default:
                    ;
            }
            break;
        }
    }
}
