/*
  +----------------------------------------------------------------------+
  | APC                                                                  |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2011 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Daniel Cowgill <dcowgill@communityconnect.com>              |
  |          Rasmus Lerdorf <rasmus@php.net>                             |
  |          Arun C. Murthy <arunc@yahoo-inc.com>                        |
  |          Gopal Vijayaraghavan <gopalv@yahoo-inc.com>                 |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.

 */

/* $Id$ */

#include "apc_cache.h"
#include "apc_zend.h"
#include "apc_sma.h"
#include "apc_globals.h"
#include "SAPI.h"
#include "TSRM.h"
#include "ext/standard/md5.h"

/* TODO: rehash when load factor exceeds threshold */

#define CHECK(p) { if ((p) == NULL) return NULL; }

/* {{{ key_equals */
#define key_equals(a, b) (a.inode==b.inode && a.device==b.device)
/* }}} */

static void apc_cache_expunge(apc_cache_t* cache, size_t size TSRMLS_DC);

/* {{{ hash */
static unsigned long hash(apc_cache_key_t key)
{
    return (unsigned long)(key.data.file.device + key.data.file.inode);
}
/* }}} */

/* {{{ string_nhash_8 */
#define string_nhash_8(s,len) (unsigned long)(zend_inline_hash_func((s), len))
/* }}} */

/* {{{ murmurhash */
#if 0
static inline unsigned long murmurhash(const char *skey, size_t keylen)
{
	const long m = 0x7fd652ad;
	const long r = 16;
	unsigned int h = 0xdeadbeef;

	while(keylen >= 4)
	{
		h += *(unsigned int*)skey;
		h *= m;
		h ^= h >> r;

		skey += 4;
		keylen -= 4;
	}

	switch(keylen)
	{
	case 3:
		h += skey[2] << 16;
	case 2:
		h += skey[1] << 8;
	case 1:
		h += skey[0];
		h *= m;
		h ^= h >> r;
	};

	h *= m;
	h ^= h >> 10;
	h *= m;
	h ^= h >> 17;

	return h;
}
#endif
/* }}} */

/* {{{ make_prime */
static int const primes[] = {
  257, /*   256 */
  521, /*   512 */
 1031, /*  1024 */
 2053, /*  2048 */
 3079, /*  3072 */
 4099, /*  4096 */
 5147, /*  5120 */
 6151, /*  6144 */
 7177, /*  7168 */
 8209, /*  8192 */
 9221, /*  9216 */
10243, /* 10240 */
11273, /* 11264 */
12289, /* 12288 */
13313, /* 13312 */
14341, /* 14336 */
15361, /* 15360 */
16411, /* 16384 */
17417, /* 17408 */
18433, /* 18432 */
19457, /* 19456 */
0      /* sentinel */
};

static int make_prime(int n)
{
    int *k = (int*)primes; 
    while(*k) {
        if((*k) > n) return *k;
        k++;
    }
    return *(k-1);
}
/* }}} */

/* {{{ make_slot */
slot_t* make_slot(apc_cache_key_t *key, apc_cache_entry_t* value, slot_t* next, time_t t TSRMLS_DC)
{
    slot_t* p = apc_pool_alloc(value->pool, sizeof(slot_t));

    if (!p) return NULL;

    if(key->type == APC_CACHE_KEY_USER) {
        char *identifier = (char*) apc_pmemcpy(key->data.user.identifier, key->data.user.identifier_len, value->pool TSRMLS_CC);
        if (!identifier) {
            return NULL;
        }
        key->data.user.identifier = identifier;
    } else if(key->type == APC_CACHE_KEY_FPFILE) {
        char *fullpath = (char*) apc_pstrdup(key->data.fpfile.fullpath, value->pool TSRMLS_CC);
        if (!fullpath) {
            return NULL;
        }
        key->data.fpfile.fullpath = fullpath;
    }
    p->key = key[0];
    p->value = value;
    p->next = next;
    p->num_hits = 0;
    p->creation_time = t;
    p->access_time = t;
    p->deletion_time = 0;
    return p;
}
/* }}} */

/* {{{ free_slot */
static void free_slot(slot_t* slot TSRMLS_DC)
{
    apc_pool_destroy(slot->value->pool TSRMLS_CC);
}
/* }}} */

/* {{{ remove_slot */
static void remove_slot(apc_cache_t* cache, slot_t** slot TSRMLS_DC)
{
    slot_t* dead = *slot;
    *slot = (*slot)->next;

    cache->header->mem_size -= dead->value->mem_size;
    CACHE_FAST_DEC(cache, cache->header->num_entries);
    if (dead->value->ref_count <= 0) {
        free_slot(dead TSRMLS_CC);
    }
    else {
        dead->next = cache->header->deleted_list;
        dead->deletion_time = time(0);
        cache->header->deleted_list = dead;
    }
}
/* }}} */

/* {{{ process_pending_removals */
static void process_pending_removals(apc_cache_t* cache TSRMLS_DC)
{
    slot_t** slot;
    time_t now;

    /* This function scans the list of removed cache entries and deletes any
     * entry whose reference count is zero (indicating that it is no longer
     * being executed) or that has been on the pending list for more than
     * cache->gc_ttl seconds (we issue a warning in the latter case).
     */

    if (!cache->header->deleted_list)
        return;

    slot = &cache->header->deleted_list;
    now = time(0);

    while (*slot != NULL) {
        int gc_sec = cache->gc_ttl ? (now - (*slot)->deletion_time) : 0;

        if ((*slot)->value->ref_count <= 0 || gc_sec > cache->gc_ttl) {
            slot_t* dead = *slot;

            if (dead->value->ref_count > 0) {
                switch(dead->value->type) {
                    case APC_CACHE_ENTRY_FILE:
                        apc_debug("GC cache entry '%s' (dev=%d ino=%d) was on gc-list for %d seconds" TSRMLS_CC, 
                            dead->value->data.file.filename, dead->key.data.file.device, dead->key.data.file.inode, gc_sec);
                        break;
                    case APC_CACHE_ENTRY_USER:
                        apc_debug("GC cache entry '%s' was on gc-list for %d seconds" TSRMLS_CC, dead->value->data.user.info, gc_sec);
                        break;
                }
            }
            *slot = dead->next;
            free_slot(dead TSRMLS_CC);
        }
        else {
            slot = &(*slot)->next;
        }
    }
}
/* }}} */

/* {{{ prevent_garbage_collection */
static void prevent_garbage_collection(apc_cache_entry_t* entry)
{
    /* set reference counts on zend objects to an arbitrarily high value to
     * prevent garbage collection after execution */

    enum { BIG_VALUE = 1000 };

    if(entry->data.file.op_array) {
        entry->data.file.op_array->refcount[0] = BIG_VALUE;
    }
    if (entry->data.file.functions) {
        int i;
        apc_function_t* fns = entry->data.file.functions;
        for (i=0; fns[i].function != NULL; i++) {
            *(fns[i].function->op_array.refcount) = BIG_VALUE;
        }
    }
    if (entry->data.file.classes) {
        int i;
        apc_class_t* classes = entry->data.file.classes;
        for (i=0; classes[i].class_entry != NULL; i++) {
            classes[i].class_entry->refcount = BIG_VALUE;
        }
    }
}
/* }}} */

/* {{{ apc_cache_create */
apc_cache_t* apc_cache_create(int size_hint, int gc_ttl, int ttl TSRMLS_DC)
{
    apc_cache_t* cache;
    int cache_size;
    int num_slots;

    num_slots = make_prime(size_hint > 0 ? size_hint : 2000);

    cache = (apc_cache_t*) apc_emalloc(sizeof(apc_cache_t) TSRMLS_CC);
    cache_size = sizeof(cache_header_t) + num_slots*sizeof(slot_t*);

    cache->shmaddr = apc_sma_malloc(cache_size TSRMLS_CC);
    if(!cache->shmaddr) {
        apc_error("Unable to allocate shared memory for cache structures.  (Perhaps your shared memory size isn't large enough?). " TSRMLS_CC);
        return NULL;
    }
    memset(cache->shmaddr, 0, cache_size);

    cache->header = (cache_header_t*) cache->shmaddr;
    cache->header->num_hits = 0;
    cache->header->num_misses = 0;
    cache->header->deleted_list = NULL;
    cache->header->start_time = time(NULL);
    cache->header->expunges = 0;
    cache->header->busy = 0;

    cache->slots = (slot_t**) (((char*) cache->shmaddr) + sizeof(cache_header_t));
    cache->num_slots = num_slots;
    cache->gc_ttl = gc_ttl;
    cache->ttl = ttl;
    CREATE_LOCK(cache->header->lock);
#if NONBLOCKING_LOCK_AVAILABLE
    CREATE_LOCK(cache->header->wrlock);
#endif
    memset(cache->slots, 0, sizeof(slot_t*)*num_slots);
    cache->expunge_cb = apc_cache_expunge;
    cache->has_lock = 0;

    return cache;
}
/* }}} */

/* {{{ apc_cache_destroy */
void apc_cache_destroy(apc_cache_t* cache TSRMLS_DC)
{
    DESTROY_LOCK(cache->header->lock);
#if NONBLOCKING_LOCK_AVAILABLE
    DESTROY_LOCK(cache->header->wrlock);
#endif

	/* XXX this is definitely a leak, but freeing this causes all the apache
		children to freeze. It might be because the segment is shared between
		several processes. To figure out is how to free this safely. */
    /*apc_sma_free(cache->shmaddr TSRMLS_CC);*/
    apc_efree(cache TSRMLS_CC);
}
/* }}} */

/* {{{ apc_cache_clear */
void apc_cache_clear(apc_cache_t* cache TSRMLS_DC)
{
    int i;

    if(!cache) return;

    CACHE_LOCK(cache);
    cache->header->busy = 1;
    cache->header->num_hits = 0;
    cache->header->num_misses = 0;
    cache->header->start_time = time(NULL);
    cache->header->expunges = 0;

    for (i = 0; i < cache->num_slots; i++) {
        slot_t* p = cache->slots[i];
        while (p) {
            remove_slot(cache, &p TSRMLS_CC);
        }
        cache->slots[i] = NULL;
    }

    memset(&cache->header->lastkey, 0, sizeof(apc_keyid_t));

    cache->header->busy = 0;
    CACHE_UNLOCK(cache);
}
/* }}} */

/* {{{ apc_cache_expunge */
static void apc_cache_expunge(apc_cache_t* cache, size_t size TSRMLS_DC)
{
    int i;
    time_t t;

    t = apc_time();

    if(!cache) return;

    if(!cache->ttl) {
        /*
         * If cache->ttl is not set, we wipe out the entire cache when
         * we run out of space.
         */
        CACHE_SAFE_LOCK(cache);
        process_pending_removals(cache TSRMLS_CC);
        if (apc_sma_get_avail_mem() > (size_t)(APCG(shm_size)/2)) {
            /* probably a queued up expunge, we don't need to do this */
            CACHE_SAFE_UNLOCK(cache);
            return;
        }
        cache->header->busy = 1;
        CACHE_FAST_INC(cache, cache->header->expunges);
clear_all:
        for (i = 0; i < cache->num_slots; i++) {
            slot_t* p = cache->slots[i];
            while (p) {
                remove_slot(cache, &p TSRMLS_CC);
            }
            cache->slots[i] = NULL;
        }
        memset(&cache->header->lastkey, 0, sizeof(apc_keyid_t));
        cache->header->busy = 0;
        CACHE_SAFE_UNLOCK(cache);
    } else {
        slot_t **p;
        /*
         * If the ttl for the cache is set we walk through and delete stale 
         * entries.  For the user cache that is slightly confusing since
         * we have the individual entry ttl's we can look at, but that would be
         * too much work.  So if you want the user cache expunged, set a high
         * default apc.user_ttl and still provide a specific ttl for each entry
         * on insert
         */

        CACHE_SAFE_LOCK(cache);
        process_pending_removals(cache TSRMLS_CC);
        if (apc_sma_get_avail_mem() > (size_t)(APCG(shm_size)/2)) {
            /* probably a queued up expunge, we don't need to do this */
            CACHE_SAFE_UNLOCK(cache);
            return;
        }
        cache->header->busy = 1;
        CACHE_FAST_INC(cache, cache->header->expunges);
        for (i = 0; i < cache->num_slots; i++) {
            p = &cache->slots[i];
            while(*p) {
                /*
                 * For the user cache we look at the individual entry ttl values
                 * and if not set fall back to the default ttl for the user cache
                 */
                if((*p)->value->type == APC_CACHE_ENTRY_USER) {
                    if((*p)->value->data.user.ttl) {
                        if((time_t) ((*p)->creation_time + (*p)->value->data.user.ttl) < t) {
                            remove_slot(cache, p TSRMLS_CC);
                            continue;
                        }
                    } else if(cache->ttl) {
                        if((*p)->creation_time + cache->ttl < t) {
                            remove_slot(cache, p TSRMLS_CC);
                            continue;
                        }
                    }
                } else if((*p)->access_time < (t - cache->ttl)) {
                    remove_slot(cache, p TSRMLS_CC);
                    continue;
                }
                p = &(*p)->next;
            }
        }

        if (!apc_sma_get_avail_size(size)) {
            /* TODO: re-do this to remove goto across locked sections */
            goto clear_all;
        }
        memset(&cache->header->lastkey, 0, sizeof(apc_keyid_t));
        cache->header->busy = 0;
        CACHE_SAFE_UNLOCK(cache);
    }
}
/* }}} */

/* {{{ apc_cache_insert */
static inline int _apc_cache_insert(apc_cache_t* cache,
                     apc_cache_key_t key,
                     apc_cache_entry_t* value,
                     apc_context_t* ctxt,
                     time_t t
                     TSRMLS_DC)
{
    slot_t** slot;

    if (!value) {
        return 0;
    }

    apc_debug("Inserting [%s]\n" TSRMLS_CC, value->data.file.filename);

    process_pending_removals(cache TSRMLS_CC);

    slot = &cache->slots[key.h % cache->num_slots];

    while(*slot) {
      if(key.type == (*slot)->key.type) {
        if(key.type == APC_CACHE_KEY_FILE) {
            if(key_equals((*slot)->key.data.file, key.data.file)) {
                /* If existing slot for the same device+inode is different, remove it and insert the new version */
                if (ctxt->force_update || (*slot)->key.mtime != key.mtime) {
                    remove_slot(cache, slot TSRMLS_CC);
                    break;
                }
                return 0;
            } else if(cache->ttl && (*slot)->access_time < (t - cache->ttl)) {
                remove_slot(cache, slot TSRMLS_CC);
                continue;
            }
        } else {   /* APC_CACHE_KEY_FPFILE */
            if((key.h == (*slot)->key.h) &&
                !memcmp((*slot)->key.data.fpfile.fullpath, key.data.fpfile.fullpath, key.data.fpfile.fullpath_len+1)) {
                /* Hrm.. it's already here, remove it and insert new one */
                remove_slot(cache, slot TSRMLS_CC);
                break;
            } else if(cache->ttl && (*slot)->access_time < (t - cache->ttl)) {
                remove_slot(cache, slot TSRMLS_CC);
                continue;
            }
        }
      }
      slot = &(*slot)->next;
    }

    if ((*slot = make_slot(&key, value, *slot, t TSRMLS_CC)) == NULL) {
        return -1;
    }

    value->mem_size = ctxt->pool->size;
    cache->header->mem_size += ctxt->pool->size;
    CACHE_FAST_INC(cache, cache->header->num_entries);
    CACHE_FAST_INC(cache, cache->header->num_inserts);

    return 1;
}
/* }}} */

/* {{{ apc_cache_insert */
int apc_cache_insert(apc_cache_t* cache, apc_cache_key_t key, apc_cache_entry_t* value, apc_context_t *ctxt, time_t t TSRMLS_DC)
{
    int rval;
    CACHE_LOCK(cache);
    rval = _apc_cache_insert(cache, key, value, ctxt, t TSRMLS_CC);
    CACHE_UNLOCK(cache);
    return rval;
}
/* }}} */

/* {{{ apc_cache_insert */
int *apc_cache_insert_mult(apc_cache_t* cache, apc_cache_key_t* keys, apc_cache_entry_t** values, apc_context_t *ctxt, time_t t, int num_entries TSRMLS_DC)
{
    int *rval;
    int i;

    rval = emalloc(sizeof(int) * num_entries);
    CACHE_LOCK(cache);
    for (i=0; i < num_entries; i++) {
        if (values[i]) {
            ctxt->pool = values[i]->pool;
            rval[i] = _apc_cache_insert(cache, keys[i], values[i], ctxt, t TSRMLS_CC);
        }
    }
    CACHE_UNLOCK(cache);
    return rval;
}
/* }}} */

/* {{{ apc_cache_user_insert */
int apc_cache_user_insert(apc_cache_t* cache, apc_cache_key_t key, apc_cache_entry_t* value, apc_context_t* ctxt, time_t t, int exclusive TSRMLS_DC)
{
    slot_t** slot;
    unsigned int keylen = key.data.user.identifier_len;
    apc_keyid_t *lastkey = &cache->header->lastkey;
    
    if (!value) {
        return 0;
    }
    
    if(apc_cache_busy(cache)) {
        /* cache cleanup in progress, do not wait */ 
        return 0;
    }

    if(apc_cache_is_last_key(cache, &key, t TSRMLS_CC)) {
        /* potential cache slam */
        return 0;
    }

    CACHE_LOCK(cache);

    memset(lastkey, 0, sizeof(apc_keyid_t));

    lastkey->h = key.h;
    lastkey->keylen = keylen;
    lastkey->mtime = t;
#ifdef ZTS
    lastkey->tid = tsrm_thread_id();
#else
    lastkey->pid = getpid();
#endif
    
    /* we do not reset lastkey after the insert. Whether it is inserted 
     * or not, another insert in the same second is always a bad idea. 
     */

    process_pending_removals(cache TSRMLS_CC);
    
    slot = &cache->slots[key.h % cache->num_slots];

    while (*slot) {
        if (((*slot)->key.h == key.h) && 
            (!memcmp((*slot)->key.data.user.identifier, key.data.user.identifier, keylen))) {
            /* 
             * At this point we have found the user cache entry.  If we are doing 
             * an exclusive insert (apc_add) we are going to bail right away if
             * the user entry already exists and it has no ttl, or
             * there is a ttl and the entry has not timed out yet.
             */
            if(exclusive && (  !(*slot)->value->data.user.ttl ||
                              ( (*slot)->value->data.user.ttl && (time_t) ((*slot)->creation_time + (*slot)->value->data.user.ttl) >= t ) 
                            ) ) {
                goto fail;
            }
            remove_slot(cache, slot TSRMLS_CC);
            break;
        } else 
        /* 
         * This is a bit nasty.  The idea here is to do runtime cleanup of the linked list of
         * slot entries so we don't always have to skip past a bunch of stale entries.  We check
         * for staleness here and get rid of them by first checking to see if the cache has a global
         * access ttl on it and removing entries that haven't been accessed for ttl seconds and secondly
         * we see if the entry has a hard ttl on it and remove it if it has been around longer than its ttl
         */
        if((cache->ttl && (*slot)->access_time < (t - cache->ttl)) || 
           ((*slot)->value->data.user.ttl && (time_t) ((*slot)->creation_time + (*slot)->value->data.user.ttl) < t)) {
            remove_slot(cache, slot TSRMLS_CC);
            continue;
        }
        slot = &(*slot)->next;
    }

    if ((*slot = make_slot(&key, value, *slot, t TSRMLS_CC)) == NULL) {
        goto fail;
    } 
    
    value->mem_size = ctxt->pool->size;
    cache->header->mem_size += ctxt->pool->size;

    CACHE_FAST_INC(cache, cache->header->num_entries);
    CACHE_FAST_INC(cache, cache->header->num_inserts);

    CACHE_UNLOCK(cache);

    return 1;

fail:
    CACHE_UNLOCK(cache);

    return 0;
}
/* }}} */

/* {{{ apc_cache_find_slot */
slot_t* apc_cache_find_slot(apc_cache_t* cache, apc_cache_key_t key, time_t t TSRMLS_DC)
{
    slot_t** slot;
    volatile slot_t* retval = NULL;

    CACHE_RDLOCK(cache);
    if(key.type == APC_CACHE_KEY_FILE) slot = &cache->slots[hash(key) % cache->num_slots];
    else slot = &cache->slots[key.h % cache->num_slots];

    while (*slot) {
      if(key.type == (*slot)->key.type) {
        if(key.type == APC_CACHE_KEY_FILE) {
            if(key_equals((*slot)->key.data.file, key.data.file)) {
                if((*slot)->key.mtime != key.mtime) {
                    #if (USE_READ_LOCKS == 0)
                    /* this is merely a memory-friendly optimization, if we do have a write-lock
                     * might as well move this to the deleted_list right-away. Otherwise an insert
                     * of the same key wil do it (or an expunge, *eventually*).
                     */
                    remove_slot(cache, slot TSRMLS_CC);
                    #endif
                    CACHE_SAFE_INC(cache, cache->header->num_misses);
                    CACHE_RDUNLOCK(cache);
                    return NULL;
                }
                CACHE_SAFE_INC(cache, (*slot)->num_hits);
                CACHE_SAFE_INC(cache, (*slot)->value->ref_count);
                (*slot)->access_time = t;
                prevent_garbage_collection((*slot)->value);
                CACHE_FAST_INC(cache, cache->header->num_hits); 
                retval = *slot;
                CACHE_RDUNLOCK(cache);
                return (slot_t*)retval;
            }
        } else {  /* APC_CACHE_KEY_FPFILE */
            if(((*slot)->key.h == key.h) &&
                !memcmp((*slot)->key.data.fpfile.fullpath, key.data.fpfile.fullpath, key.data.fpfile.fullpath_len+1)) {
                /* TTL Check ? */
                CACHE_SAFE_INC(cache, (*slot)->num_hits);
                CACHE_SAFE_INC(cache, (*slot)->value->ref_count);
                (*slot)->access_time = t;
                prevent_garbage_collection((*slot)->value);
                CACHE_FAST_INC(cache, cache->header->num_hits);
                retval = *slot;
                CACHE_RDUNLOCK(cache);
                return (slot_t*)retval;
            }
        }
      }
      slot = &(*slot)->next;
    }
    CACHE_FAST_INC(cache, cache->header->num_misses); 
    CACHE_RDUNLOCK(cache);
    return NULL;
}
/* }}} */

/* {{{ apc_cache_find */
apc_cache_entry_t* apc_cache_find(apc_cache_t* cache, apc_cache_key_t key, time_t t TSRMLS_DC)
{
    slot_t * slot = apc_cache_find_slot(cache, key, t TSRMLS_CC);
    apc_debug("apc_cache_find [%i]\n" TSRMLS_CC, key.h);
    return (slot) ? slot->value : NULL;
}
/* }}} */

/* {{{ apc_cache_user_find */
apc_cache_entry_t* apc_cache_user_find(apc_cache_t* cache, char *strkey, int keylen, time_t t TSRMLS_DC)
{
    slot_t** slot;
    volatile apc_cache_entry_t* value = NULL;
    unsigned long h;

    if(apc_cache_busy(cache))
    {
        /* cache cleanup in progress */ 
        return NULL;
    }

    CACHE_RDLOCK(cache);

    h = string_nhash_8(strkey, keylen);

    slot = &cache->slots[h % cache->num_slots];

    while (*slot) {
        if ((h == (*slot)->key.h) &&
            !memcmp((*slot)->key.data.user.identifier, strkey, keylen)) {
            /* Check to make sure this entry isn't expired by a hard TTL */
            if((*slot)->value->data.user.ttl && (time_t) ((*slot)->creation_time + (*slot)->value->data.user.ttl) < t) {
                #if (USE_READ_LOCKS == 0) 
                /* this is merely a memory-friendly optimization, if we do have a write-lock
                 * might as well move this to the deleted_list right-away. Otherwise an insert
                 * of the same key wil do it (or an expunge, *eventually*).
                 */
                remove_slot(cache, slot TSRMLS_CC);
                #endif
                CACHE_FAST_INC(cache, cache->header->num_misses);
                CACHE_RDUNLOCK(cache);
                return NULL;
            }
            /* Otherwise we are fine, increase counters and return the cache entry */
            CACHE_SAFE_INC(cache, (*slot)->num_hits);
            CACHE_SAFE_INC(cache, (*slot)->value->ref_count);
            (*slot)->access_time = t;

            CACHE_FAST_INC(cache, cache->header->num_hits);
            value = (*slot)->value;
            CACHE_RDUNLOCK(cache);
            return (apc_cache_entry_t*)value;
        }
        slot = &(*slot)->next;
    }
 
    CACHE_FAST_INC(cache, cache->header->num_misses);
    CACHE_RDUNLOCK(cache);
    return NULL;
}
/* }}} */

/* {{{ apc_cache_user_exists */
apc_cache_entry_t* apc_cache_user_exists(apc_cache_t* cache, char *strkey, int keylen, time_t t TSRMLS_DC)
{
    slot_t** slot;
    volatile apc_cache_entry_t* value = NULL;
    unsigned long h;

    if(apc_cache_busy(cache))
    {
        /* cache cleanup in progress */ 
        return NULL;
    }

    CACHE_RDLOCK(cache);

    h = string_nhash_8(strkey, keylen);

    slot = &cache->slots[h % cache->num_slots];

    while (*slot) {
        if ((h == (*slot)->key.h) &&
            !memcmp((*slot)->key.data.user.identifier, strkey, keylen)) {
            /* Check to make sure this entry isn't expired by a hard TTL */
            if((*slot)->value->data.user.ttl && (time_t) ((*slot)->creation_time + (*slot)->value->data.user.ttl) < t) {
                CACHE_UNLOCK(cache);
                return NULL;
            }
            /* Return the cache entry ptr */
            value = (*slot)->value;
            CACHE_RDUNLOCK(cache);
            return (apc_cache_entry_t*)value;
        }
        slot = &(*slot)->next;
    }
    CACHE_RDUNLOCK(cache);
    return NULL;
}
/* }}} */

/* {{{ apc_cache_user_update */
int _apc_cache_user_update(apc_cache_t* cache, char *strkey, int keylen, apc_cache_updater_t updater, void* data TSRMLS_DC)
{
    slot_t** slot;
    int retval;
    unsigned long h;

    if(apc_cache_busy(cache))
    {
        /* cache cleanup in progress */ 
        return 0;
    }

    CACHE_LOCK(cache);

    h = string_nhash_8(strkey, keylen);
    slot = &cache->slots[h % cache->num_slots];

    while (*slot) {
        if ((h == (*slot)->key.h) &&
            !memcmp((*slot)->key.data.user.identifier, strkey, keylen)) {
            switch(Z_TYPE_P((*slot)->value->data.user.val) & ~IS_CONSTANT_INDEX) {
                case IS_ARRAY:
                case IS_CONSTANT_ARRAY:
                case IS_OBJECT:
                {
                    if(APCG(serializer)) {
                        retval = 0;
                        break;
                    } else {
                        /* fall through */
                    }
                }
                /* fall through */
                default:
                {
                    retval = updater(cache, (*slot)->value, data);
                    (*slot)->key.mtime = apc_time();
                }
                break;
            }
            CACHE_UNLOCK(cache);
            return retval;
        }
        slot = &(*slot)->next;
    }
    CACHE_UNLOCK(cache);
    return 0;
}
/* }}} */

/* {{{ apc_cache_user_delete */
int apc_cache_user_delete(apc_cache_t* cache, char *strkey, int keylen TSRMLS_DC)
{
    slot_t** slot;
    unsigned long h;

    CACHE_LOCK(cache);

    h = string_nhash_8(strkey, keylen);

    slot = &cache->slots[h % cache->num_slots];

    while (*slot) {
        if ((h == (*slot)->key.h) && 
            !memcmp((*slot)->key.data.user.identifier, strkey, keylen)) {
            remove_slot(cache, slot TSRMLS_CC);
            CACHE_UNLOCK(cache);
            return 1;
        }
        slot = &(*slot)->next;
    }

    CACHE_UNLOCK(cache);
    return 0;
}
/* }}} */

/* {{{ apc_cache_delete */
int apc_cache_delete(apc_cache_t* cache, char *filename, int filename_len TSRMLS_DC)
{
    slot_t** slot;
    time_t t;
    apc_cache_key_t key;

    t = apc_time();

    /* try to create a cache key; if we fail, give up on caching */
    if (!apc_cache_make_file_key(&key, filename, PG(include_path), t TSRMLS_CC)) {
        apc_warning("Could not stat file %s, unable to delete from cache." TSRMLS_CC, filename);
        return -1;
    }

    CACHE_LOCK(cache);

    if(key.type == APC_CACHE_KEY_FILE) slot = &cache->slots[hash(key) % cache->num_slots];
    else slot = &cache->slots[key.h % cache->num_slots];

    while(*slot) {
      if(key.type == (*slot)->key.type) {
        if(key.type == APC_CACHE_KEY_FILE) {
            if(key_equals((*slot)->key.data.file, key.data.file)) {
                remove_slot(cache, slot TSRMLS_CC);
                CACHE_UNLOCK(cache);
                return 1;
            }
        } else {   /* APC_CACHE_KEY_FPFILE */
            if(((*slot)->key.h == key.h) &&
                (!memcmp((*slot)->key.data.fpfile.fullpath, key.data.fpfile.fullpath, key.data.fpfile.fullpath_len+1))) {
                remove_slot(cache, slot TSRMLS_CC);
                CACHE_UNLOCK(cache);
                return 1;
            }
        }
      }
      slot = &(*slot)->next;
    }
    
    memset(&cache->header->lastkey, 0, sizeof(apc_keyid_t));
    
    CACHE_UNLOCK(cache);
    return 0;

}
/* }}} */

/* {{{ apc_cache_release */
void apc_cache_release(apc_cache_t* cache, apc_cache_entry_t* entry TSRMLS_DC)
{
    CACHE_SAFE_DEC(cache, entry->ref_count);
}
/* }}} */

/* {{{ apc_cache_make_file_key */
int apc_cache_make_file_key(apc_cache_key_t* key,
                       const char* filename,
                       const char* include_path,
                       time_t t
                       TSRMLS_DC)
{
    struct stat *tmp_buf=NULL;
    struct apc_fileinfo_t *fileinfo = NULL;
    int len;

    assert(key != NULL);

    if (!filename || !SG(request_info).path_translated) {
        apc_debug("No filename and no path_translated - bailing\n" TSRMLS_CC);
        goto cleanup;
    }

    memset(key->md5, 0, 16);

    len = strlen(filename);
    if(APCG(fpstat)==0) {
        if(IS_ABSOLUTE_PATH(filename,len) || strstr(filename, "://")) {
            key->data.fpfile.fullpath = filename;
            key->data.fpfile.fullpath_len = len;
            key->h = string_nhash_8((char *)key->data.fpfile.fullpath, key->data.fpfile.fullpath_len);
            key->mtime = t;
            key->type = APC_CACHE_KEY_FPFILE;
            goto success;
        } else if(APCG(canonicalize)) {

            fileinfo = apc_php_malloc(sizeof(apc_fileinfo_t) TSRMLS_CC);

            if (apc_search_paths(filename, include_path, fileinfo TSRMLS_CC) != 0) {
                apc_warning("apc failed to locate %s - bailing" TSRMLS_CC, filename);
                goto cleanup;
            }

            if(!VCWD_REALPATH(fileinfo->fullpath, APCG(canon_path))) {
                apc_warning("realpath failed to canonicalize %s - bailing" TSRMLS_CC, filename);
                goto cleanup;
            }

            key->data.fpfile.fullpath = APCG(canon_path);
            key->data.fpfile.fullpath_len = strlen(APCG(canon_path));
            key->h = string_nhash_8((char *)key->data.fpfile.fullpath, key->data.fpfile.fullpath_len);
            key->mtime = t;
            key->type = APC_CACHE_KEY_FPFILE;
            goto success;
        }
        /* fall through to stat mode */
    }

    fileinfo = apc_php_malloc(sizeof(apc_fileinfo_t) TSRMLS_CC);

    assert(fileinfo != NULL);

    if(!strcmp(SG(request_info).path_translated, filename)) {
        tmp_buf = sapi_get_stat(TSRMLS_C);  /* Apache has already done this stat() for us */
    }

    if(tmp_buf) {
        fileinfo->st_buf.sb = *tmp_buf;
    } else {
        if (apc_search_paths(filename, include_path, fileinfo TSRMLS_CC) != 0) {
            apc_debug("Stat failed %s - bailing (%s) (%d)\n" TSRMLS_CC, filename,SG(request_info).path_translated);
            goto cleanup;
        }
    }

    if(APCG(max_file_size) < fileinfo->st_buf.sb.st_size) {
        apc_debug("File is too big %s (%d - %ld) - bailing\n" TSRMLS_CC, filename,t,fileinfo->st_buf.sb.st_size);
        goto cleanup;
    }

    /*
     * This is a bit of a hack.
     *
     * Here I am checking to see if the file is at least 2 seconds old.  
     * The idea is that if the file is currently being written to then its
     * mtime is going to match or at most be 1 second off of the current
     * request time and we want to avoid caching files that have not been
     * completely written.  Of course, people should be using atomic 
     * mechanisms to push files onto live web servers, but adding this
     * tiny safety is easier than educating the world.  This is now
     * configurable, but the default is still 2 seconds.
     */
    if(APCG(file_update_protection) && (t - fileinfo->st_buf.sb.st_mtime < APCG(file_update_protection)) && !APCG(force_file_update)) {
        apc_debug("File is too new %s (%d - %d) - bailing\n" TSRMLS_CC,filename,t,fileinfo->st_buf.sb.st_mtime);
        goto cleanup;
    }

    key->data.file.device = fileinfo->st_buf.sb.st_dev;
    key->data.file.inode  = fileinfo->st_buf.sb.st_ino;
    key->h = (unsigned long) key->data.file.device + (unsigned long) key->data.file.inode;

    /*
     * If working with content management systems that like to munge the mtime, 
     * it might be appropriate to key off of the ctime to be immune to systems
     * that try to backdate a template.  If the mtime is set to something older
     * than the previous mtime of a template we will obviously never see this
     * "older" template.  At some point the Smarty templating system did this.
     * I generally disagree with using the ctime here because you lose the 
     * ability to warm up new content by saving it to a temporary file, hitting
     * it once to cache it and then renaming it into its permanent location so
     * set the apc.stat_ctime=true to enable this check.
     */
    if(APCG(stat_ctime)) {
        key->mtime  = (fileinfo->st_buf.sb.st_ctime > fileinfo->st_buf.sb.st_mtime) ? fileinfo->st_buf.sb.st_ctime : fileinfo->st_buf.sb.st_mtime; 
    } else {
        key->mtime = fileinfo->st_buf.sb.st_mtime;
    }
    key->type = APC_CACHE_KEY_FILE;

success: 

    if(fileinfo != NULL) {
        apc_php_free(fileinfo TSRMLS_CC);
    }

    return 1;

cleanup:
    
    if(fileinfo != NULL) {
        apc_php_free(fileinfo TSRMLS_CC);
    }

    return 0;
}
/* }}} */

/* {{{ apc_cache_make_user_key */
int apc_cache_make_user_key(apc_cache_key_t* key, char* identifier, int identifier_len, const time_t t)
{
    assert(key != NULL);

    if (!identifier)
        return 0;

    key->data.user.identifier = identifier;
    key->data.user.identifier_len = identifier_len;
    key->h = string_nhash_8((char *)key->data.user.identifier, key->data.user.identifier_len);
    key->mtime = t;
    key->type = APC_CACHE_KEY_USER;
    return 1;
}
/* }}} */

/* {{{ apc_cache_make_file_entry */
apc_cache_entry_t* apc_cache_make_file_entry(const char* filename,
                                        zend_op_array* op_array,
                                        apc_function_t* functions,
                                        apc_class_t* classes,
                                        apc_context_t* ctxt
                                        TSRMLS_DC)
{
    apc_cache_entry_t* entry;
    apc_pool* pool = ctxt->pool;

    entry = (apc_cache_entry_t*) apc_pool_alloc(pool, sizeof(apc_cache_entry_t));
    if (!entry) return NULL;

    entry->data.file.filename  = apc_pstrdup(filename, pool TSRMLS_CC);
    if(!entry->data.file.filename) {
        apc_debug("apc_cache_make_file_entry: entry->data.file.filename is NULL - bailing\n" TSRMLS_CC);
        return NULL;
    }
    apc_debug("apc_cache_make_file_entry: entry->data.file.filename is [%s]\n" TSRMLS_CC,entry->data.file.filename);
    entry->data.file.op_array  = op_array;
    entry->data.file.functions = functions;
    entry->data.file.classes   = classes;

    entry->data.file.halt_offset = apc_file_halt_offset(filename TSRMLS_CC);

    entry->type = APC_CACHE_ENTRY_FILE;
    entry->ref_count = 0;
    entry->mem_size = 0;
    entry->pool = pool;
    return entry;
}
/* }}} */

/* {{{ apc_cache_store_zval */
zval* apc_cache_store_zval(zval* dst, const zval* src, apc_context_t* ctxt TSRMLS_DC)
{
    if (Z_TYPE_P(src) == IS_ARRAY) {
        /* Maintain a list of zvals we've copied to properly handle recursive structures */
        zend_hash_init(&APCG(copied_zvals), 0, NULL, NULL, 0);
        dst = apc_copy_zval(dst, src, ctxt TSRMLS_CC);
        zend_hash_destroy(&APCG(copied_zvals));
        APCG(copied_zvals).nTableSize=0;
    } else {
        dst = apc_copy_zval(dst, src, ctxt TSRMLS_CC);
    }


    return dst;
}
/* }}} */

/* {{{ apc_cache_fetch_zval */
zval* apc_cache_fetch_zval(zval* dst, const zval* src, apc_context_t* ctxt TSRMLS_DC)
{
    if (Z_TYPE_P(src) == IS_ARRAY) {
        /* Maintain a list of zvals we've copied to properly handle recursive structures */
        zend_hash_init(&APCG(copied_zvals), 0, NULL, NULL, 0);
        dst = apc_copy_zval(dst, src, ctxt TSRMLS_CC);
        zend_hash_destroy(&APCG(copied_zvals));
        APCG(copied_zvals).nTableSize=0;
    } else {
        dst = apc_copy_zval(dst, src, ctxt TSRMLS_CC);
    }


    return dst;
}
/* }}} */

/* {{{ apc_cache_make_user_entry */
apc_cache_entry_t* apc_cache_make_user_entry(const char* info, int info_len, const zval* val, apc_context_t* ctxt, const unsigned int ttl TSRMLS_DC)
{
    apc_cache_entry_t* entry;
    apc_pool* pool = ctxt->pool;

    entry = (apc_cache_entry_t*) apc_pool_alloc(pool, sizeof(apc_cache_entry_t));
    if (!entry) return NULL;

    entry->data.user.info = apc_pmemcpy(info, info_len, pool TSRMLS_CC);
    entry->data.user.info_len = info_len;
    if(!entry->data.user.info) {
        return NULL;
    }
    entry->data.user.val = apc_cache_store_zval(NULL, val, ctxt TSRMLS_CC);
    if(!entry->data.user.val) {
        return NULL;
    }
    INIT_PZVAL(entry->data.user.val);
    entry->data.user.ttl = ttl;
    entry->type = APC_CACHE_ENTRY_USER;
    entry->ref_count = 0;
    entry->mem_size = 0;
    entry->pool = pool;
    return entry;
}
/* }}} */

/* {{{ apc_cache_link_info */
static zval* apc_cache_link_info(apc_cache_t *cache, slot_t* p TSRMLS_DC)
{
    zval *link = NULL;
    char md5str[33];

    ALLOC_INIT_ZVAL(link);

    if(!link) {
        return NULL;
    }

    array_init(link);

    if(p->value->type == APC_CACHE_ENTRY_FILE) {
        add_assoc_string(link, "type", "file", 1);
        if(p->key.type == APC_CACHE_KEY_FILE) {

            #ifdef PHP_WIN32
            {
              char buf[20];
              sprintf(buf, "%I64d",  p->key.data.file.device);
              add_assoc_string(link, "device", buf, 1);

              sprintf(buf, "%I64d",  p->key.data.file.inode);
              add_assoc_string(link, "inode", buf, 1);
            }
            #else
            add_assoc_long(link, "device", p->key.data.file.device);
            add_assoc_long(link, "inode", p->key.data.file.inode);
            #endif

            add_assoc_string(link, "filename", p->value->data.file.filename, 1);
        } else { /* This is a no-stat fullpath file entry */
            add_assoc_long(link, "device", 0);
            add_assoc_long(link, "inode", 0);
            add_assoc_string(link, "filename", (char*)p->key.data.fpfile.fullpath, 1);
        }
        if (APCG(file_md5)) {
               make_digest(md5str, p->key.md5);
               add_assoc_string(link, "md5", md5str, 1);
        } 
    } else if(p->value->type == APC_CACHE_ENTRY_USER) {
        add_assoc_stringl(link, "info", p->value->data.user.info, p->value->data.user.info_len-1, 1);
        add_assoc_long(link, "ttl", (long)p->value->data.user.ttl);
        add_assoc_string(link, "type", "user", 1);
    }

    add_assoc_double(link, "num_hits", (double)p->num_hits);
    add_assoc_long(link, "mtime", p->key.mtime);
    add_assoc_long(link, "creation_time", p->creation_time);
    add_assoc_long(link, "deletion_time", p->deletion_time);
    add_assoc_long(link, "access_time", p->access_time);
    add_assoc_long(link, "ref_count", p->value->ref_count);
    add_assoc_long(link, "mem_size", p->value->mem_size);

    return link;
}
/* }}} */

/* {{{ apc_cache_info */
zval* apc_cache_info(apc_cache_t* cache, zend_bool limited TSRMLS_DC)
{
    zval *info = NULL;
    zval *list = NULL;
    zval *deleted_list = NULL;
    zval *slots = NULL;
    slot_t* p;
    int i, j;

    if(!cache) return NULL;

    CACHE_RDLOCK(cache);

    ALLOC_INIT_ZVAL(info);

    if(!info) {
        CACHE_RDUNLOCK(cache);
        return NULL;
    }

    array_init(info);
    add_assoc_long(info, "num_slots", cache->num_slots);
    add_assoc_long(info, "ttl", cache->ttl);

    add_assoc_double(info, "num_hits", (double)cache->header->num_hits);
    add_assoc_double(info, "num_misses", (double)cache->header->num_misses);
    add_assoc_double(info, "num_inserts", (double)cache->header->num_inserts);
    add_assoc_double(info, "expunges", (double)cache->header->expunges);
    
    add_assoc_long(info, "start_time", cache->header->start_time);
    add_assoc_double(info, "mem_size", (double)cache->header->mem_size);
    add_assoc_long(info, "num_entries", cache->header->num_entries);
#ifdef MULTIPART_EVENT_FORMDATA
    add_assoc_long(info, "file_upload_progress", 1);
#else
    add_assoc_long(info, "file_upload_progress", 0);
#endif
#if APC_MMAP
    add_assoc_stringl(info, "memory_type", "mmap", sizeof("mmap")-1, 1);
#else
    add_assoc_stringl(info, "memory_type", "IPC shared", sizeof("IPC shared")-1, 1);
#endif
    add_assoc_stringl(info, "locking_type", APC_LOCK_TYPE, sizeof(APC_LOCK_TYPE)-1, 1);

    if(!limited) {
        /* For each hashtable slot */
        ALLOC_INIT_ZVAL(list);
        array_init(list);

        ALLOC_INIT_ZVAL(slots);
        array_init(slots);

        for (i = 0; i < cache->num_slots; i++) {
            p = cache->slots[i];
            j = 0;
            for (; p != NULL; p = p->next) {
                zval *link = apc_cache_link_info(cache, p TSRMLS_CC);
                add_next_index_zval(list, link);
                j++;
            }
            if(j != 0) {
                add_index_long(slots, (ulong)i, j);
            }
        }

        /* For each slot pending deletion */
        ALLOC_INIT_ZVAL(deleted_list);
        array_init(deleted_list);

        for (p = cache->header->deleted_list; p != NULL; p = p->next) {
            zval *link = apc_cache_link_info(cache, p TSRMLS_CC);
            add_next_index_zval(deleted_list, link);
        }
        
        add_assoc_zval(info, "cache_list", list);
        add_assoc_zval(info, "deleted_list", deleted_list);
        add_assoc_zval(info, "slot_distribution", slots);
    }

    CACHE_RDUNLOCK(cache);
    return info;
}
/* }}} */

/* {{{ apc_cache_unlock */
void apc_cache_unlock(apc_cache_t* cache TSRMLS_DC)
{
    CACHE_UNLOCK(cache);
}
/* }}} */

/* {{{ apc_cache_busy */
zend_bool apc_cache_busy(apc_cache_t* cache)
{
    return cache->header->busy;
}
/* }}} */

/* {{{ apc_cache_is_last_key */
zend_bool apc_cache_is_last_key(apc_cache_t* cache, apc_cache_key_t* key, time_t t TSRMLS_DC)
{
    apc_keyid_t *lastkey = &cache->header->lastkey;
    unsigned int keylen = key->data.user.identifier_len;
#ifdef ZTS
    THREAD_T tid = tsrm_thread_id();
    #define FROM_DIFFERENT_THREAD(k) (memcmp(&((k)->tid), &tid, sizeof(THREAD_T))!=0)
#else
    pid_t pid = getpid();
    #define FROM_DIFFERENT_THREAD(k) (pid != (k)->pid) 
#endif


    /* unlocked reads, but we're not shooting for 100% success with this */
    if(lastkey->h == key->h && keylen == lastkey->keylen) {
        if(lastkey->mtime == t && FROM_DIFFERENT_THREAD(lastkey)) {
            /* potential cache slam */
            if(APCG(slam_defense)) {
                apc_debug("Potential cache slam averted for key '%s'" TSRMLS_CC, key->data.user.identifier);
                return 1;
            }
        }
    }

    return 0;
}
/* }}} */

#if NONBLOCKING_LOCK_AVAILABLE
/* {{{ apc_cache_write_lock */
zend_bool apc_cache_write_lock(apc_cache_t* cache TSRMLS_DC)
{
    return apc_lck_nb_lock(cache->header->wrlock);
}
/* }}} */

/* {{{ apc_cache_write_unlock */
void apc_cache_write_unlock(apc_cache_t* cache TSRMLS_DC)
{
    apc_lck_unlock(cache->header->wrlock);
}
/* }}} */
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
