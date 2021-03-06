/*
 * Copyright 2017 Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "serialize.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lib.h"
#include "logging.h"
#include "script.h"

struct svalue {
        int type;
        union {
                bool boolean;
                lua_Number number;
                char *string;
                struct sfunction *function;
                struct stable *table;
        };
};

struct supvalue {
        struct supvalue *next;
        void *id;
        int number;
        struct svalue value;
        void *collector_id;
};

struct stable_entry {
        struct stable_entry *next;
        struct svalue key;
        struct svalue value;
        /* Collector object for the entry value */
        void *collector_id;
};

struct stable {
        void *id;
        struct stable_entry *entries;
};

struct sfunction {
        void *id;
        struct byte_array *code;
        struct supvalue *upvalues;
};

struct upvalue_mapping {
        struct upvalue_mapping *next;
        void *key;
        void *function_id;
        int upvalue_num;
};

struct object_mapping {
        struct object_mapping *next;
        void *key;
        void *object_id;
};

/* Keyed by collector object ID */
struct collector_mapping {
        struct collector_mapping *next;
        /* collector object from which value was extracted */
        void *key;
        /* function or table */
        void *object_id;
        /* upvalue number or table key */
        struct svalue value_key;
};

struct upvalue_cache {
        /* Map of serialized upvalue ids to deserialized (function id, upvalue
         * number) tuples */
        struct upvalue_mapping *upvalue_map;
        /* Map of serialized object ids to deserialized object ids */
        struct object_mapping *object_map;
        /* Map of collector object ids to (function, upvalue number) or (table, key) pairs */
        struct collector_mapping *collector_map;
        /* Lua store for deserialized objects indexed by their id */
        int object_tbl_idx;
};


static void free_value_data(struct svalue *v);
static void serialize_object(struct callbacks *cb, lua_State *L,
                             struct svalue *object);
static int push_function(struct callbacks *cb, lua_State *L,
                         struct upvalue_cache *cache,
                         const struct sfunction *func, const char *name,
                         void **object_key);
static void push_object(struct callbacks *cb, lua_State *L,
                        struct upvalue_cache *cache,
                        const struct svalue *object);


static void free_table_entry(struct stable_entry *e)
{
        free_value_data(&e->key);
        free_value_data(&e->value);
        free(e);
}

static void free_table_entries(struct stable_entry *entries)
{
        struct stable_entry *e;

        LIST_FOR_EACH (entries, e)
                free_table_entry(e);
}

static void free_table(struct stable *t)
{
        if (t) {
                free_table_entries(t->entries);
                free(t);
        }
}

static void free_value_data(struct svalue *v)
{
        if (!v)
                return;

        switch (v->type) {
        case LUA_TBOOLEAN:
        case LUA_TNUMBER:
                /* nothing to do */
                break;
        case LUA_TSTRING:
                free(v->string);
                break;
        case LUA_TFUNCTION:
                free_sfunction(v->function);
                break;
        case LUA_TTABLE:
                free_table(v->table);
                break;
        default:
                assert(false);
        }
}

static struct supvalue *l_upvalue_new(void *id, int number)
{
        struct supvalue *v;

        v = calloc(1, sizeof(*v));
        assert(v);

        v->id = id;
        v->number = number;

        return v;
}

static void free_upvalue(struct supvalue *v)
{
        if (!v)
                return;

        free_value_data(&v->value);
        free(v);
}

/**
 * Frees a list of upvalues. List head pointer gets reset to NULL.
 */
static void free_upvalues(struct supvalue **head)
{
        struct supvalue *v;

        assert(head);

        LIST_FOR_EACH (*head, v)
                free_upvalue(v);
        *head = NULL;
}

void free_sfunction(struct sfunction *f)
{
        if (f) {
                byte_array_free(f->code);
                free_upvalues(&f->upvalues);
                free(f);
        }
}

static int string_writer(lua_State *L, const void *str, size_t len, void *buf)
{
        UNUSED(L);
        luaL_addlstring(buf, str, len);
        return 0;
}

static struct byte_array *dump_function_bytecode(struct callbacks *cb,
                                                 lua_State *L)
{
        struct byte_array *code;
        const char *buf;
        size_t len = 0;
        luaL_Buffer B;
        int err;

        luaL_buffinit(L, &B);
        err = lua_dump(L, string_writer, &B);
        if (err)
                LOG_FATAL(cb, "lua_dump: %s", lua_tostring(L, -1));
        luaL_pushresult(&B);
        buf = lua_tolstring(L, -1, &len);
        if (!buf || !len)
                LOG_FATAL(cb, "lua_dump returned an empty buffer");

        code = byte_array_new((uint8_t *) buf, len);
        lua_pop(L, 1);

        return code;
}

static int load_function_bytecode(struct callbacks *cb, lua_State *L,
                                  const struct byte_array *bytecode,
                                  const char *name)
{
        int err;

        err = luaL_loadbuffer(L, (char *) bytecode->data, bytecode->len, name);
        if (err) {
                LOG_FATAL(cb, "%s: luaL_loadbuffer: %s",
                          name, lua_tostring(L, -1));
                return -errno_lua(err);
        }

        return 0;
}

/* Pushes the first element of the table onto the stack, if the object on top of
 * a stack is a table that has a 'collector' meta-field.
 */
static void *unwrap_collector_maybe(lua_State *L)
{
        void *collector_id = NULL;
        bool exists;

        exists = luaL_getmetafield(L, -1, "collector");
        if (exists) {
                /* field value doesn't matter */
                lua_pop(L, 1);

                collector_id = (void *) lua_topointer(L, -1);

                /* push wrapped value */
                lua_rawgeti(L, -1, 1);

                /* remove wrapper */
                lua_remove(L, -2);
        }

        return collector_id;
}

static struct stable_entry *dump_table_entries(struct callbacks *cb,
                                                lua_State *L)
{
        struct stable_entry *head = NULL;

        lua_pushnil(L);
        while (lua_next(L, -2)) {
                struct stable_entry *e = calloc(1, sizeof(*e));
                if (!e)
                        LOG_FATAL(cb, "calloc failed");
                e->next = head;
                head = e;

                e->collector_id = unwrap_collector_maybe(L);
                serialize_object(cb, L, &e->value);
                lua_pop(L, 1);
                serialize_object(cb, L, &e->key);
                /* leave key on stack */
        }

        return head;
}

static struct stable *serialize_table(struct callbacks *cb, lua_State *L)
{
        struct stable *t;

        t = calloc(1, sizeof(*t));
        assert(t);

        t->id = (void *) lua_topointer(L, -1);
        t->entries = dump_table_entries(cb, L);

        return t;
}

static void serialize_object(struct callbacks *cb, lua_State *L,
                             struct svalue *object)
{
        object->type = lua_type(L, -1);

        switch (object->type) {
        case LUA_TNIL:
                assert(false);
                break;
        case LUA_TNUMBER:
                object->number = lua_tonumber(L, -1);
                break;
        case LUA_TBOOLEAN:
                object->boolean = lua_toboolean(L, -1);
                break;
        case LUA_TSTRING:
                object->string = strdup(lua_tostring(L, -1));
                break;
        case LUA_TTABLE:
                object->table = serialize_table(cb, L);
                break;
        case LUA_TFUNCTION:
                object->function = serialize_function(cb, L);
                break;
        case LUA_TUSERDATA:
                assert(false); /* XXX: Not implemented */
                break;
        case LUA_TTHREAD:
                assert(false); /* XXX: Not implemented */
                break;
        case LUA_TLIGHTUSERDATA:
                assert(false); /* XXX: Not implemented */
                break;
        default:
                assert(false);
        }
}

/**
 * Serializes an upvalue. Expects the upvalue to be at the top of the stack.
 * Takes the upvalue's number for use during deserialization at a later time.
 */
struct supvalue *serialize_upvalue(struct callbacks *cb, lua_State *L,
                                    void *id, int number)
{
        struct supvalue *v;

        v = l_upvalue_new(id, number);
        v->collector_id = unwrap_collector_maybe(L);
        serialize_object(cb, L, &v->value);

        return v;
}

/**
 * Inserts a given upvale at the begining of a list.
 */
void prepend_upvalue(struct supvalue **head, struct supvalue *upvalue)
{
        assert(head);
        assert(upvalue);

        upvalue->next = *head;
        *head = upvalue;
}

static struct supvalue *serialize_upvalues(struct callbacks *cb, lua_State *L)
{
        struct supvalue *list = NULL;
        struct supvalue *v;
        void *v_id;
        int i;

        for (i = 1; lua_getupvalue(L, -1, i); i++) {
                v_id = lua_upvalueid(L, -2, i);
                v = serialize_upvalue(cb, L, v_id, i);
                prepend_upvalue(&list, v);
                lua_pop(L, 1);
        }

        return list;
}

struct sfunction *serialize_function(struct callbacks *cb, lua_State *L)
{
        struct sfunction *f;

        f = calloc(1, sizeof(*f));
        assert(f);

        f->id = (void *) lua_topointer(L, -1);
        f->code = dump_function_bytecode(cb, L);
        f->upvalues = serialize_upvalues(cb, L);

        return f;
}

static void map_object(struct upvalue_cache *cache, void *key,
                       void *object_id)
{
        struct object_mapping *m;

        m = calloc(1, sizeof(*m));
        assert(m);
        m->key = key;
        m->object_id = object_id;

        m->next = cache->object_map;
        cache->object_map = m;
}

static void copy_value(const struct svalue *src, struct svalue *dst)
{
        assert(src->type == LUA_TBOOLEAN ||
               src->type == LUA_TNUMBER ||
               src->type == LUA_TSTRING);

        memcpy(dst, src, sizeof(*dst));
        if (src->type == LUA_TSTRING)
                dst->string = strdup(src->string);
}

static void map_collector(struct upvalue_cache *cache,
                          void *key, void *object_id,
                          const struct svalue *value_key)
{
        struct collector_mapping *m;

        m = calloc(1, sizeof(*m));
        assert(m);
        m->key = key;
        m->object_id = object_id;
        copy_value(value_key, &m->value_key);

        m->next = cache->collector_map;
        cache->collector_map = m;
}

static const struct object_mapping *lookup_object(struct upvalue_cache *cache,
                                                  void *key)
{
        struct object_mapping *m;

        LIST_FOR_EACH (cache->object_map, m) {
                if (m->key == key)
                        return m;
        }
        return NULL;
}

static void *cache_object(struct upvalue_cache *cache, lua_State *L)
{
        void *id;

        id = (void *) lua_topointer(L, -1);
        lua_pushlightuserdata(L, id);
        lua_pushvalue(L, -2);
        lua_rawset(L, cache->object_tbl_idx);

        return id;
}

static void fetch_object(struct upvalue_cache *cache, lua_State *L, void *id)
{
        lua_pushlightuserdata(L, id);
        lua_rawget(L, cache->object_tbl_idx);
}

static bool lookup_and_fetch_object(struct upvalue_cache *cache, lua_State *L,
                                    void *key)
{
        const struct object_mapping *m;

        m = lookup_object(cache, key);
        if (m) {
                lua_pushlightuserdata(L, m->object_id);
                lua_rawget(L, cache->object_tbl_idx);
                return true;
        }
        return false;
}

static void map_upvalue(struct upvalue_cache *cache,
                        const struct supvalue *upvalue, void *function_id)
{
        struct upvalue_mapping *m;

        m = calloc(1, sizeof(*m));
        assert(m);
        m->key = upvalue->id;
        m->function_id = function_id;
        m->upvalue_num = upvalue->number;

        m->next = cache->upvalue_map;
        cache->upvalue_map = m;
}

static const struct upvalue_mapping *lookup_upvalue(struct upvalue_cache *cache,
                                                    void *key)
{
        struct upvalue_mapping *m;

        LIST_FOR_EACH (cache->upvalue_map, m) {
                if (m->key == key)
                        return m;
        }
        return NULL;
}

static void push_table(struct callbacks *cb, lua_State *L,
                       struct upvalue_cache *cache,
                       struct stable *table)
{
        struct stable_entry *e;
        void *tid;

        lua_newtable(L);

        tid = cache_object(cache, L);
        map_object(cache, table->id, tid);

        LIST_FOR_EACH (table->entries, e) {
                push_object(cb, L, cache, &e->key);
                push_object(cb, L, cache, &e->value);
                lua_rawset(L, -3);

                if (e->collector_id) {
                        map_collector(cache, e->collector_id,
                                      tid, &e->key);
                }
        }
}

static void push_object(struct callbacks *cb, lua_State *L,
                        struct upvalue_cache *cache,
                        const struct svalue *object)
{
        switch (object->type) {
        case LUA_TBOOLEAN:
                lua_pushboolean(L, object->boolean);
                break;
        case LUA_TNUMBER:
                lua_pushnumber(L, object->number);
                break;
        case LUA_TSTRING:
                lua_pushstring(L, object->string);
                break;
        case LUA_TFUNCTION:
                if (!lookup_and_fetch_object(cache, L, object->function->id))
                    push_function(cb, L, cache, object->function, NULL, NULL);
                break;
        case LUA_TTABLE:
                if (!lookup_and_fetch_object(cache, L, object->table->id))
                    push_table(cb, L, cache, object->table);
                break;
        default:
                assert(false);
                break;
        }
}

static void set_upvalue(struct callbacks *cb, lua_State *L,
                        struct upvalue_cache *cache,
                        const struct supvalue *upvalue)
{
        const char *name;

        push_object(cb, L, cache, &upvalue->value);
        name = lua_setupvalue(L, -2, upvalue->number);
        assert(name);
}

struct upvalue_cache *upvalue_cache_new(void)
{
        return calloc(1, sizeof(struct upvalue_cache));
}

static void free_object_mappings(struct object_mapping *mappings)
{
        struct object_mapping *m;

        LIST_FOR_EACH (mappings, m)
                free(m);
}

static void free_upvalue_mappings(struct upvalue_mapping *mappings)
{
        struct upvalue_mapping *m;

        LIST_FOR_EACH (mappings, m)
                free(m);
}

static void free_collector_mappings(struct collector_mapping *mappings)
{
        struct collector_mapping *m;

        LIST_FOR_EACH (mappings, m) {
                free_value_data(&m->value_key);
                free(m);
        }
}

void free_upvalue_cache(struct upvalue_cache *c)
{
        if (c) {
                free_object_mappings(c->object_map);
                free_upvalue_mappings(c->upvalue_map);
                free_collector_mappings(c->collector_map);
                free(c);
        }
}

/**
 * Deserializes an upvalue value and sets it as an upvalue of a function
 * identified by func_id.
 *
 * Records each upvalue deserialized for the first time in the cache table
 * located at given index on the stack. If an upvalue has been deserialized
 * before, it will be reused the next time it is encountered via
 * lua_upvaluejoin().
 */
static void set_shared_upvalue(struct callbacks *cb, lua_State *L,
                               struct upvalue_cache *upvalue_cache,
                               void *func_id, const struct supvalue *upvalue)
{
        const struct upvalue_mapping *m;

        m = lookup_upvalue(upvalue_cache, upvalue->id);
        if (m) {
                /* An already seen upvalue, we're sharing */
                fetch_object(upvalue_cache, L, m->function_id);
                lua_upvaluejoin(L, -2, upvalue->number, -1, m->upvalue_num);
                lua_pop(L, 1);
        } else {
                /* Upvalue seen for the first time */
                set_upvalue(cb, L, upvalue_cache, upvalue);
                map_upvalue(upvalue_cache, upvalue, func_id);

                if (upvalue->collector_id) {
                        struct svalue vkey = {
                                .type = LUA_TNUMBER,
                                .number = upvalue->number,
                        };
                        map_collector(upvalue_cache, upvalue->collector_id,
                                      func_id, &vkey);
                }
        }
}

static int push_function(struct callbacks *cb, lua_State *L,
                         struct upvalue_cache *cache,
                         const struct sfunction *func, const char *name,
                         void **object_key)
{
        struct supvalue *v;
        void *func_id;
        int err;

        assert(cache);
        assert(func);

        err = load_function_bytecode(cb, L, func->code, name);
        if (err)
                return err;

        func_id = cache_object(cache, L);
        map_object(cache, func->id, func_id);

        /* Set upvalues */
        LIST_FOR_EACH (func->upvalues, v)
                set_shared_upvalue(cb, L, cache, func_id, v);

        /* TODO: Push globals */

        if (object_key)
                *object_key = func_id;

        return 0;
}

int deserialize_function(struct callbacks *cb, lua_State *L,
                         struct upvalue_cache *cache, int cache_idx,
                         const struct sfunction *func, const char *name,
                         void **object_key)
{
        assert(cache);
        assert(func);
        assert(object_key);

        cache->object_tbl_idx = cache_idx;

        return push_function(cb, L, cache, func, name, object_key);
}

struct svalue *serialize_value(struct callbacks *cb, lua_State *L)
{
        struct svalue *sv;

        sv = calloc(1, sizeof(*sv));
        assert(sv);
        serialize_object(cb, L, sv);

        return sv;
}

void deserialize_value(struct callbacks *cb, lua_State *L,
                       struct upvalue_cache *cache, int cache_idx,
                       const struct svalue *value)
{
        cache->object_tbl_idx = cache_idx;
        push_object(cb, L, cache, value);
}

void free_svalue(struct svalue *value)
{
        if (value) {
                free_value_data(value);
                free(value);
        }
}

void push_collected_value(struct callbacks *cb, lua_State *L,
                          struct upvalue_cache *cache, int cache_idx,
                          void *collector_id)
{
        struct collector_mapping *m;

        cache->object_tbl_idx = cache_idx;

        LIST_FOR_EACH (cache->collector_map, m) {
                if (m->key != collector_id)
                        continue;

                fetch_object(cache, L, m->object_id);
                if (lua_isfunction(L, -1)) {
                        const char *name;

                        name = lua_getupvalue(L, -1, m->value_key.number);
                        assert(name);
                } else if (lua_istable(L, -1)) {
                        push_object(cb, L, cache, &m->value_key);
                        lua_rawget(L, -2);
                } else {
                        LOG_FATAL(cb, "Expected function or table object but got %s",
                                  lua_typename(L, lua_type(L, -1)));
                }
                lua_remove(L, -2); /* fetched object */
                return; /* got it */
        }

        lua_pushnil(L); /* not found */
}
