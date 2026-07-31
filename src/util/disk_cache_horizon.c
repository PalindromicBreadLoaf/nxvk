/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 */

/* Horizon has no mmap() and no advisory file locking, so neither of Mesa's
 * file-backed cache layouts can be brought up. This is a small append-only
 * store behind disk_cache's blob callbacks instead.
 *
 * Records are only ever appended, so a run that dies mid-write leaves a tail
 * past the committed count that the next scan ignores and the next write
 * overwrites.
 */

#include "disk_cache_horizon.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util/crc32.h"
#include "util/disk_cache.h"
#include "util/hash_table.h"
#include "util/log.h"
#include "util/os_misc.h"
#include "util/simple_mtx.h"

#define STORE_MAGIC   0x4353414d /* 'MASC' */
#define STORE_VERSION 1

struct store_header {
   uint32_t magic;
   uint32_t version;
   uint64_t used; /* bytes of the file holding committed records */
};

struct store_record {
   uint8_t key[CACHE_KEY_SIZE];
   uint32_t size;
   uint32_t crc;
};

struct store_entry {
   uint8_t key[CACHE_KEY_SIZE];
   uint64_t offset; /* payload */
   uint32_t size;
   uint32_t crc;
};

static struct {
   simple_mtx_t mtx;
   FILE *file;
   struct hash_table *index;
   uint64_t used;
   uint64_t max_size;
   unsigned refcount;
   bool full;
} store = { .mtx = SIMPLE_MTX_INITIALIZER };

static uint32_t
key_hash(const void *key)
{
   return _mesa_hash_data(key, CACHE_KEY_SIZE);
}

static bool
key_equal(const void *a, const void *b)
{
   return memcmp(a, b, CACHE_KEY_SIZE) == 0;
}

static void
entry_free(struct hash_entry *he)
{
   free(he->data);
}

static void
index_put(const uint8_t *key, uint64_t offset, uint32_t size, uint32_t crc)
{
   struct hash_entry *he = _mesa_hash_table_search(store.index, key);
   struct store_entry *e;

   if (he != NULL) {
      e = he->data;
   } else {
      e = malloc(sizeof(*e));
      if (e == NULL)
         return;

      memcpy(e->key, key, CACHE_KEY_SIZE);
      _mesa_hash_table_insert(store.index, e->key, e);
   }

   e->offset = offset;
   e->size = size;
   e->crc = crc;
}

static bool
store_commit(void)
{
   const struct store_header hdr = {
      .magic = STORE_MAGIC,
      .version = STORE_VERSION,
      .used = store.used,
   };

   return fseek(store.file, 0, SEEK_SET) == 0 &&
          fwrite(&hdr, sizeof(hdr), 1, store.file) == 1 &&
          fflush(store.file) == 0;
}

/* Rebuild the index by walking record headers. Anything past the committed
 * count, or a record that runs off the end of it, is a torn tail.
 */
static bool
store_scan(void)
{
   struct store_header hdr;

   store.used = sizeof(hdr);

   if (fseek(store.file, 0, SEEK_SET) != 0)
      return false;

   if (fread(&hdr, sizeof(hdr), 1, store.file) != 1 ||
       hdr.magic != STORE_MAGIC || hdr.version != STORE_VERSION ||
       hdr.used < sizeof(hdr))
      return store_commit();

   uint64_t off = sizeof(hdr);
   while (off < hdr.used) {
      struct store_record rec;

      if (fseek(store.file, off, SEEK_SET) != 0 ||
          fread(&rec, sizeof(rec), 1, store.file) != 1)
         break;

      const uint64_t next = off + sizeof(rec) + rec.size;
      if (rec.size == 0 || next > hdr.used)
         break;

      index_put(rec.key, off + sizeof(rec), rec.size, rec.crc);
      off = next;
   }

   store.used = off;

   return off == hdr.used || store_commit();
}

/* mkdir -p */
static bool
make_dir(const char *path)
{
   char *tmp = strdup(path);
   if (tmp == NULL)
      return false;

   char *p = strchr(tmp, ':');
   p = p != NULL ? p + 1 : tmp;
   if (*p == '/')
      p++;

   bool ok = true;
   for (; *p != '\0' && ok; p++) {
      if (*p != '/')
         continue;

      *p = '\0';
      ok = mkdir(tmp, 0755) == 0 || errno == EEXIST;
      *p = '/';
   }

   ok = ok && (mkdir(tmp, 0755) == 0 || errno == EEXIST);

   free(tmp);
   return ok;
}

static bool
store_open(uint64_t max_size)
{
   const char *base = os_get_option("MESA_SHADER_CACHE_DIR");
   if (base == NULL)
      base = "sdmc:/switch";

   char *dir = NULL;
   if (asprintf(&dir, "%s/%s", base, CACHE_DIR_NAME) < 0)
      return false;

   bool ok = false;
   char *path = NULL;

   if (!make_dir(dir)) {
      mesa_logw("shader cache: cannot create %s", dir);
      goto out;
   }

   if (asprintf(&path, "%s/cache.bin", dir) < 0)
      goto out;

   /* "r+b" keeps an existing store. */
   store.file = fopen(path, "r+b");
   if (store.file == NULL)
      store.file = fopen(path, "w+b");
   if (store.file == NULL) {
      mesa_logw("shader cache: cannot open %s", path);
      goto out;
   }

   store.index = _mesa_hash_table_create(NULL, key_hash, key_equal);
   if (store.index == NULL)
      goto out;

   if (!store_scan()) {
      mesa_logw("shader cache: %s is unusable", path);
      goto out;
   }

   store.max_size = max_size;
   ok = true;

   mesa_logi("shader cache: %s, %u entries, %" PRIu64 " KiB",
             path, _mesa_hash_table_num_entries(store.index),
             store.used / 1024);

out:
   if (!ok) {
      if (store.index != NULL) {
         _mesa_hash_table_destroy(store.index, entry_free);
         store.index = NULL;
      }
      if (store.file != NULL) {
         fclose(store.file);
         store.file = NULL;
      }
   }

   free(path);
   free(dir);
   return ok;
}

static void
blob_put(const void *key, signed long key_size,
         const void *value, signed long value_size)
{
   if (key_size != CACHE_KEY_SIZE || value_size <= 0 ||
       value_size > DISK_CACHE_HORIZON_MAX_BLOB)
      return;

   simple_mtx_lock(&store.mtx);

   if (store.file == NULL)
      goto unlock;

   const uint64_t need = sizeof(struct store_record) + (uint64_t)value_size;
   if (store.used + need > store.max_size) {
      if (!store.full) {
         store.full = true;
         mesa_logi("shader cache: full at %" PRIu64 " KiB, no longer growing",
                   store.used / 1024);
      }
      goto unlock;
   }

   struct store_record rec;
   memcpy(rec.key, key, CACHE_KEY_SIZE);
   rec.size = (uint32_t)value_size;
   rec.crc = util_hash_crc32(value, value_size);

   if (fseek(store.file, store.used, SEEK_SET) != 0 ||
       fwrite(&rec, sizeof(rec), 1, store.file) != 1 ||
       fwrite(value, value_size, 1, store.file) != 1 ||
       fflush(store.file) != 0)
      goto unlock;

   const uint64_t payload = store.used + sizeof(rec);
   const uint64_t prev_used = store.used;

   /* Committing the header is what publishes the record. */
   store.used += need;
   if (!store_commit()) {
      store.used = prev_used;
      goto unlock;
   }

   index_put(rec.key, payload, rec.size, rec.crc);

unlock:
   simple_mtx_unlock(&store.mtx);
}

static signed long
blob_get(const void *key, signed long key_size,
         void *value, signed long value_size)
{
   signed long ret = 0;

   if (key_size != CACHE_KEY_SIZE)
      return 0;

   simple_mtx_lock(&store.mtx);

   if (store.file == NULL)
      goto unlock;

   const struct hash_entry *he = _mesa_hash_table_search(store.index, key);
   if (he == NULL)
      goto unlock;

   const struct store_entry *e = he->data;

   /* Caller's buffer is too small. */
   if ((uint64_t)e->size > (uint64_t)value_size)
      goto unlock;

   if (fseek(store.file, e->offset, SEEK_SET) != 0 ||
       fread(value, e->size, 1, store.file) != 1)
      goto unlock;

   if (util_hash_crc32(value, e->size) != e->crc)
      goto unlock;

   ret = e->size;

unlock:
   simple_mtx_unlock(&store.mtx);
   return ret;
}

void
disk_cache_horizon_init(struct disk_cache *cache, uint64_t max_size)
{
   bool ok;

   simple_mtx_lock(&store.mtx);

   ok = store.refcount > 0 || store_open(max_size);
   if (ok)
      store.refcount++;

   simple_mtx_unlock(&store.mtx);

   if (ok)
      disk_cache_set_callbacks(cache, blob_put, blob_get);
}

void
disk_cache_horizon_fini(void)
{
   simple_mtx_lock(&store.mtx);

   assert(store.refcount > 0);
   if (--store.refcount == 0 && store.file != NULL) {
      _mesa_hash_table_destroy(store.index, entry_free);
      store.index = NULL;
      fclose(store.file);
      store.file = NULL;
      store.full = false;
   }

   simple_mtx_unlock(&store.mtx);
}
