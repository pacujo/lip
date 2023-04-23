#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <assert.h>
#include <encjson.h>
#include <fsdyn/charstr.h>
#include <fsdyn/hashtable.h>
#include <fsdyn/integer.h>
#include "util.h"
#include "intl.h"
#include "i18n.h"

#ifndef PREFIX
#define PREFIX /usr/local
#endif

static hash_table_t *translations; /* string -> string */
static hash_table_t *interned;     /* uintptr_t -> string */

static void add_translations(json_thing_t *i18n, const char *lang)
{
    assert(json_thing_type(i18n) == JSON_OBJECT);
    for (json_field_t *field = json_object_first(i18n); field;
         field = json_field_next(field)) {
        json_thing_t *langs = json_field_value(field);
        assert(json_thing_type(langs) == JSON_OBJECT);
        const char *translation;
        if (json_object_get_string(langs, lang, &translation)) {
            hash_elem_t *he =
                hash_table_put(translations,
                               charstr_dupstr(json_field_name(field)),
                               charstr_dupstr(translation));
            if (he) {           /* overridden */
                fsfree((char *) hash_elem_get_key(he));
                fsfree((char *) hash_elem_get_value(he));
                destroy_hash_element(he);
            }
        }
    }
}

static int i18n_filter(const struct dirent *entity)
{
    return charstr_ends_with(entity->d_name, ".json");
}

static void import_shared_i18n(const char *lang, const char *dirpath)
{
    struct dirent **namelist;
    int n = scandir(dirpath, &namelist, i18n_filter, NULL);
    if (n < 0)
        return;
    for (int i = 0; i < n; i++) {
        char *path = charstr_printf("%s/%s", dirpath, namelist[i]->d_name);
        free(namelist[i]);
        size_t count;
        char *content = read_file(path, &count);
        fsfree(path);
        if (!content)
            continue;
        json_thing_t *i18n = json_utf8_decode(content, count);
        fsfree(content);
        if (!i18n)
            continue;
        if (json_thing_type(i18n) == JSON_OBJECT)
            add_translations(i18n, lang);
        json_destroy_thing(i18n);
    }
    free(namelist);
}

static void import_global_i18n(const char *lang)
{
    char *dirpath = charstr_printf("%s/share/lip/i18n", stringify(PREFIX));
    import_shared_i18n(lang, dirpath);
    fsfree(dirpath);
}

static void import_local_i18n(const char *lang)
{
    const char *home = getenv("HOME");
    if (!home)
        return;
    char *dirpath = charstr_printf("%s/.local/share/lip/i18n", home);
    import_shared_i18n(lang, dirpath);
    fsfree(dirpath);
}

static void initialize()
{
    const char *lang = getenv("LANG");
    if (!lang) {
        translations =
            make_hash_table(1, (void *) hash_string, (void *) strcmp);
        interned =
            make_hash_table(1, (void *) hash_unsigned, (void *) unsigned_cmp);
        return;
    }
    json_thing_t *i18n = json_utf8_decode_string(i18n_json);
    assert(json_thing_type(i18n) == JSON_OBJECT);
    translations = make_hash_table(1000, (void *) hash_string, (void *) strcmp);
    unsigned count = 0;
    for (json_field_t *field = json_object_first(i18n); field;
         field = json_field_next(field), count++)
        ;
    add_translations(i18n, lang);
    json_destroy_thing(i18n);
    interned =
        make_hash_table(count, (void *) hash_unsigned, (void *) unsigned_cmp);
    import_global_i18n(lang);
    import_local_i18n(lang);
}

const char *_(const char *s)
{
    if (!translations)
        initialize();
    unsigned_t *intern_key = as_unsigned((uintptr_t) s);
    hash_elem_t *he = hash_table_get(interned, intern_key);
    if (he)
        return hash_elem_get_value(he);
    he = hash_table_get(translations, s);
    if (he) {
        const char *translation = hash_elem_get_value(he);
        hash_table_put(interned, intern_key, translation);
        return translation;
    }
    hash_table_put(interned, intern_key, s);
    return s;
}
