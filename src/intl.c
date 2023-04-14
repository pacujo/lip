#include <string.h>
#include <stdbool.h>
#include <encjson.h>
#include <assert.h>
#include <fsdyn/charstr.h>
#include <fsdyn/hashtable.h>
#include <fsdyn/integer.h>
#include "intl.h"
#include "i18n.h"

static hash_table_t *translations; /* string -> string */
static hash_table_t *interned;     /* uintptr_t -> string */

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
         field = json_field_next(field), count++) {
        json_thing_t *langs = json_field_value(field);
        assert(json_thing_type(langs) == JSON_OBJECT);
        const char *translation;
        if (json_object_get_string(langs, lang, &translation)) {
            hash_elem_t *he =
                hash_table_put(translations,
                               charstr_dupstr(json_field_name(field)),
                               charstr_dupstr(translation));
            if (he) {           /* duplicate! */
                fsfree((char *) hash_elem_get_key(he));
                fsfree((char *) hash_elem_get_value(he));
                destroy_hash_element(he);
            }
        }
    }
    json_destroy_thing(i18n);
    interned =
        make_hash_table(count, (void *) hash_unsigned, (void *) unsigned_cmp);
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
