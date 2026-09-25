/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Horizon has no mmap() and no advisory file locking, so neither of Mesa's
 * file-backed cache layouts can be brought up. This is a small per-driver version,
 * append-only store behind disk_cache's blob callbacks instead.
 */

#include "disk_cache_horizon.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/crc32.h"
#include "util/disk_cache.h"
#include "util/hash_table.h"
#include "util/log.h"
#include "util/macros.h"
#include "util/mesa-blake3.h"
#include "util/os_misc.h"
#include "util/os_time.h"
#include "util/simple_mtx.h"

#define STORE_MAGIC   0x4353414d /* 'MASC' */
#define STORE_VERSION 2
#define INDEX_MAGIC   0x4953414d /* 'MASI' */
#define INDEX_VERSION 1

#define STORE_DIR_NAME ".mesa_shader_cache"

#define LEGACY_DIR_NAME CACHE_DIR_NAME
#define LEGACY_STORE    "cache.bin"

#define STORE_NAME_HEX 16
#define SCAN_CHUNK     (256 * 1024)

struct store_header {
   uint32_t magic;
   uint32_t version;
   uint64_t used;
   uint64_t id;
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

struct index_header {
   uint32_t magic;
   uint32_t version;
   uint64_t store_id;
   uint64_t used;
   uint32_t count;
   uint32_t crc;
};

static struct {
   simple_mtx_t mtx;
   FILE *file;
   char *index_path;
   struct hash_table *index;
   uint64_t id;
   uint64_t used;
   uint64_t indexed;
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
      .id = store.id,
   };

   return fseek(store.file, 0, SEEK_SET) == 0 &&
          fwrite(&hdr, sizeof(hdr), 1, store.file) == 1 &&
          fflush(store.file) == 0;
}

static void
index_write(void)
{
   const uint32_t count = _mesa_hash_table_num_entries(store.index);
   struct store_entry *entries = malloc(MAX2(count, 1) * sizeof(*entries));
   if (entries == NULL)
      return;

   uint32_t i = 0;
   hash_table_foreach(store.index, he)
      entries[i++] = *(const struct store_entry *)he->data;

   const struct index_header ih = {
      .magic = INDEX_MAGIC,
      .version = INDEX_VERSION,
      .store_id = store.id,
      .used = store.used,
      .count = count,
      .crc = util_hash_crc32(entries, count * sizeof(*entries)),
   };

   FILE *f = fopen(store.index_path, "wb");
   if (f != NULL) {
      bool ok = fwrite(&ih, sizeof(ih), 1, f) == 1 &&
                fwrite(entries, sizeof(*entries), count, f) == count;
      ok = fclose(f) == 0 && ok;
      if (ok)
         store.indexed = store.used;
   }

   free(entries);
}

/* Returns the store bytes the index covers, or 0 if it cannot be trusted. */
static uint64_t
index_load(const struct store_header *hdr)
{
   FILE *f = fopen(store.index_path, "rb");
   if (f == NULL)
      return 0;

   struct index_header ih;
   struct store_entry *entries = NULL;
   uint64_t covered = 0;

   if (fread(&ih, sizeof(ih), 1, f) != 1 ||
       ih.magic != INDEX_MAGIC || ih.version != INDEX_VERSION ||
       ih.store_id != hdr->id ||
       ih.used < sizeof(*hdr) || ih.used > hdr->used)
      goto out;

   entries = malloc(MAX2(ih.count, 1) * sizeof(*entries));
   if (entries == NULL ||
       fread(entries, sizeof(*entries), ih.count, f) != ih.count ||
       util_hash_crc32(entries, (size_t)ih.count * sizeof(*entries)) != ih.crc)
      goto out;

   for (uint32_t i = 0; i < ih.count; i++) {
      const struct store_entry *e = &entries[i];
      if (e->offset < sizeof(*hdr) + sizeof(struct store_record) ||
          e->offset + e->size > ih.used)
         goto out;
   }

   for (uint32_t i = 0; i < ih.count; i++)
      index_put(entries[i].key, entries[i].offset, entries[i].size,
                entries[i].crc);

   covered = ih.used;

out:
   free(entries);
   fclose(f);
   return covered;
}

static uint64_t
store_scan(uint64_t off, uint64_t end)
{
   uint8_t *buf = malloc(SCAN_CHUNK);
   if (buf == NULL)
      return off;

   uint64_t buf_off = 0;
   size_t buf_len = 0;

   while (off < end) {
      struct store_record rec;

      if (off < buf_off || off + sizeof(rec) > buf_off + buf_len) {
         if (fseek(store.file, off, SEEK_SET) != 0)
            break;

         buf_off = off;
         buf_len = fread(buf, 1, MIN2(end - off, SCAN_CHUNK), store.file);
         if (buf_len < sizeof(rec))
            break;
      }

      memcpy(&rec, buf + (off - buf_off), sizeof(rec));

      const uint64_t next = off + sizeof(rec) + rec.size;
      if (rec.size == 0 || next > end)
         break;

      index_put(rec.key, off + sizeof(rec), rec.size, rec.crc);
      off = next;
   }

   free(buf);
   return off;
}

static bool
store_load(void)
{
   struct store_header hdr;

   if (fseek(store.file, 0, SEEK_SET) != 0)
      return false;

   if (fread(&hdr, sizeof(hdr), 1, store.file) != 1 ||
       hdr.magic != STORE_MAGIC || hdr.version != STORE_VERSION ||
       hdr.used < sizeof(hdr)) {
      store.id = (uint64_t)os_time_get_nano() ^ (uintptr_t)&store;
      store.used = sizeof(hdr);
      store.indexed = 0;
      return store_commit();
   }

   store.id = hdr.id;
   store.indexed = index_load(&hdr);

   store.used = store_scan(MAX2(store.indexed, sizeof(hdr)), hdr.used);
   if (store.used != hdr.used && !store_commit())
      return false;

   if (store.used != store.indexed)
      index_write();

   return true;
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
is_store_name(const char *name)
{
   if (strlen(name) != STORE_NAME_HEX + 4 ||
       strcmp(name + STORE_NAME_HEX, ".bin") != 0)
      return false;

   for (unsigned i = 0; i < STORE_NAME_HEX; i++) {
      if (!isxdigit((unsigned char)name[i]))
         return false;
   }

   return true;
}

struct other_store {
   char *bin;
   char *idx;
   uint64_t size;
   time_t last_used;
};

static int
cmp_last_used(const void *a, const void *b)
{
   const struct other_store *x = a, *y = b;
   return (x->last_used > y->last_used) - (x->last_used < y->last_used);
}

static void
unlink_path(const char *dir, const char *name)
{
   char *path = NULL;
   if (asprintf(&path, "%s/%s", dir, name) >= 0) {
      unlink(path);
      free(path);
   }
}

static void
remove_legacy_dir(const char *base)
{
   char *dir = NULL;
   if (asprintf(&dir, "%s/%s", base, LEGACY_DIR_NAME) < 0)
      return;

   DIR *d = opendir(dir);
   if (d != NULL) {
      struct dirent *de;
      while ((de = readdir(d)) != NULL) {
         const size_t len = strlen(de->d_name);
         if (strcmp(de->d_name, LEGACY_STORE) == 0 ||
             is_store_name(de->d_name) ||
             (len == STORE_NAME_HEX + 4 &&
              strcmp(de->d_name + STORE_NAME_HEX, ".idx") == 0))
            unlink_path(dir, de->d_name);
      }
      closedir(d);

      rmdir(dir);
   }

   free(dir);
}

static void
evict_other_stores(const char *dir, const char *own, uint64_t budget)
{
   DIR *d = opendir(dir);
   if (d == NULL)
      return;

   struct other_store *others = NULL;
   unsigned count = 0, cap = 0;
   uint64_t total = 0;
   struct dirent *de;

   while ((de = readdir(d)) != NULL) {
      if (!is_store_name(de->d_name) || strcmp(de->d_name, own) == 0)
         continue;

      if (count == cap) {
         cap = MAX2(cap * 2, 16);
         struct other_store *grown = realloc(others, cap * sizeof(*others));
         if (grown == NULL)
            break;
         others = grown;
      }

      struct other_store *o = &others[count];
      struct stat st;

      if (asprintf(&o->bin, "%s/%s", dir, de->d_name) < 0)
         continue;
      if (asprintf(&o->idx, "%s/%.*s.idx", dir, STORE_NAME_HEX,
                   de->d_name) < 0) {
         free(o->bin);
         continue;
      }
      if (stat(o->bin, &st) != 0) {
         free(o->bin);
         free(o->idx);
         continue;
      }

      o->size = st.st_size;
      o->last_used = st.st_mtime;
      if (stat(o->idx, &st) == 0)
         o->size += st.st_size;

      total += o->size;
      count++;
   }

   closedir(d);

   if (total > budget) {
      qsort(others, count, sizeof(*others), cmp_last_used);

      for (unsigned i = 0; i < count && total > budget; i++) {
         unlink(others[i].idx);
         if (unlink(others[i].bin) == 0)
            total -= others[i].size;
      }
   }

   for (unsigned i = 0; i < count; i++) {
      free(others[i].bin);
      free(others[i].idx);
   }
   free(others);
}

static bool
store_open(const void *driver_keys, size_t driver_keys_size,
           uint64_t max_size)
{
   const char *base = os_get_option("MESA_SHADER_CACHE_DIR");
   if (base == NULL)
      base = "sdmc:/switch";

   blake3_hash hash;
   _mesa_blake3_compute(driver_keys, driver_keys_size, hash);

   char name[STORE_NAME_HEX + 5];
   for (unsigned i = 0; i < STORE_NAME_HEX / 2; i++)
      snprintf(name + 2 * i, 3, "%02x", hash[i]);
   strcpy(name + STORE_NAME_HEX, ".bin");

   char *dir = NULL;
   if (asprintf(&dir, "%s/%s", base, STORE_DIR_NAME) < 0)
      return false;

   bool ok = false;
   char *path = NULL;

   if (!make_dir(dir)) {
      mesa_logw("shader cache: cannot create %s", dir);
      goto out;
   }

   if (asprintf(&path, "%s/%s", dir, name) < 0 ||
       asprintf(&store.index_path, "%s/%.*s.idx", dir, STORE_NAME_HEX,
                name) < 0)
      goto out;

   store.file = fopen(path, "r+b");
   if (store.file == NULL) {
      store.file = fopen(path, "w+b");
      if (store.file != NULL) {
         remove_legacy_dir(base);
         evict_other_stores(dir, name, max_size);
      }
   }
   if (store.file == NULL) {
      mesa_logw("shader cache: cannot open %s", path);
      goto out;
   }

   store.index = _mesa_hash_table_create(NULL, key_hash, key_equal);
   if (store.index == NULL)
      goto out;

   if (!store_load()) {
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
      free(store.index_path);
      store.index_path = NULL;
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
disk_cache_horizon_init(struct disk_cache *cache, uint64_t max_size,
                        const void *driver_keys, size_t driver_keys_size)
{
   bool ok;

   simple_mtx_lock(&store.mtx);

   ok = store.refcount > 0 ||
        store_open(driver_keys, driver_keys_size, max_size);
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
      if (store.used != store.indexed)
         index_write();

      store_commit();

      _mesa_hash_table_destroy(store.index, entry_free);
      store.index = NULL;
      fclose(store.file);
      store.file = NULL;
      free(store.index_path);
      store.index_path = NULL;
      store.full = false;
   }

   simple_mtx_unlock(&store.mtx);
}
