/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real production store open/append/scan/index bodies over a real store and
 * index file, so the on-disk consequence of the publication policy is tested
 * end to end: what a process that closes the store cleanly publishes, what a
 * process that dies after appending leaves behind, and what the next process
 * recovers from the card. The hash table is a fixture; the record, header and
 * index formats are the production ones. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "util/disk_cache_horizon.h"
#define CACHE_KEY_SIZE 20
#define MIN2(a,b) ((a) < (b) ? (a) : (b))
#define MAX2(a,b) ((a) > (b) ? (a) : (b))
#include "cache_defines.inc"
#include "cache_structs.inc"

#define INDEX_SLOTS 64

struct hash_entry { void *data; };
struct hash_table {
   unsigned count;
   struct hash_entry hes[INDEX_SLOTS];
};

static struct {
   unsigned mtx;
   FILE *file;
   char *index_path;
   struct hash_table *index;
   uint64_t used, indexed, max_size, id;
   uint64_t commit_pending_B;
   bool full;
   bool commit_per_blob;
} store;
static struct store_stats stats;
static struct hash_table table;
static unsigned write_calls, read_calls, flush_calls, crc_calls, log_messages;
static char store_path[4096], index_path[4096];
static uint64_t next_id = 17;

static struct hash_entry *table_search(struct hash_table *t, const void *key)
{
   for (unsigned i = 0; i < t->count; i++) {
      const struct store_entry *e = t->hes[i].data;
      if (memcmp(e->key, key, CACHE_KEY_SIZE) == 0)
         return &t->hes[i];
   }
   return NULL;
}
static struct hash_entry *_mesa_hash_table_search(struct hash_table *t, const void *key)
{
   assert(store.mtx == 1);
   return table_search(t, key);
}
static void _mesa_hash_table_insert(struct hash_table *t, const void *key, void *data)
{
   assert(store.mtx == 1);
   assert(t->count < INDEX_SLOTS);
   assert(table_search(t, key) == NULL);
   t->hes[t->count++].data = data; /* the entry owns its key, as upstream does */
}
static uint32_t _mesa_hash_table_num_entries(struct hash_table *t) { return t->count; }
/* The extracted index_write() walks the table with this macro; the fixture
 * table is an insertion-ordered array, which is all that loop needs. */
#define hash_table_foreach(t, entry) \
   for (struct hash_entry *entry = (t)->hes; entry != (t)->hes + (t)->count; entry++)

static struct store_entry *entry_for(const uint8_t *key)
{
   struct hash_entry *he = table_search(&table, key);
   return he != NULL ? he->data : NULL;
}

static void simple_mtx_lock(unsigned *m) { assert(*m == 0); *m = 1; }
static void simple_mtx_unlock(unsigned *m) { assert(*m == 1); *m = 0; }
static uint32_t util_hash_crc32(const void *value, size_t size) { crc_calls++; return 0x12345678; }
static uint64_t os_time_get_nano(void) { return 0x9e3779b9; }
#define mesa_logi(...) ((void)++log_messages)
#define mesa_logw(...) ((void)++log_messages)
static size_t observed_write(const void *p, size_t size, size_t n, FILE *f)
{
   write_calls++;
   return fwrite(p, size, n, f);
}
static size_t observed_read(void *p, size_t size, size_t n, FILE *f)
{
   read_calls++;
   return fread(p, size, n, f);
}
static int observed_flush(FILE *f) { flush_calls++; return fflush(f); }
#define fwrite observed_write
#define fread observed_read
#define fflush observed_flush
#include "cache_load.inc"
#undef fwrite
#undef fread
#undef fflush

static void counters(void)
{
   write_calls = read_calls = flush_calls = crc_calls = log_messages = 0;
}

/* The fixture owns the table lifecycle that disk_cache_horizon.c keeps in
 * store_open()/disk_cache_horizon_fini(). */
static void table_clear(void)
{
   for (unsigned i = 0; i < table.count; i++)
      entry_free(&table.hes[i]);
   memset(&table, 0, sizeof(table));
}

/* A process starting on an existing store: fresh index, reopened file. */
static void reopen(bool commit_per_blob)
{
   table_clear();
   store.index = &table;
   store.file = fopen(store_path, "r+b");
   assert(store.file != NULL);
   store.commit_per_blob = commit_per_blob;
   /* Production opens the store under the store lock. */
   simple_mtx_lock(&store.mtx);
   const bool ok = store_load();
   simple_mtx_unlock(&store.mtx);
   assert(ok);
}

/* A process that dies without closing the store: no index write, no header
 * publication, no teardown of any kind. */
static void crash(void)
{
   fclose(store.file);
   store.file = NULL;
   store.index = NULL;
   store.index_path = index_path;
}

static void create_store(uint64_t capacity, bool commit_per_blob)
{
   memset(&store, 0, sizeof(store));
   table_clear();
   store.file = fopen(store_path, "w+b");
   assert(store.file != NULL);
   store.index = &table;
   store.index_path = index_path;
   store.id = next_id++;
   store.used = sizeof(struct store_header);
   store.indexed = 0;
   store.max_size = capacity;
   store.commit_per_blob = commit_per_blob;
   simple_mtx_lock(&store.mtx);
   const bool committed = store_commit();
   simple_mtx_unlock(&store.mtx);
   assert(committed);
   stats = (struct store_stats) { 0 };
   counters();
}

static uint64_t header_used(void)
{
   struct store_header hdr;
   FILE *f = fopen(store_path, "rb");
   assert(f != NULL);
   assert(fread(&hdr, sizeof(hdr), 1, f) == 1);
   fclose(f);
   assert(hdr.magic == STORE_MAGIC && hdr.version == STORE_VERSION);
   return hdr.used;
}

static void verify_blob(const uint8_t *key, const uint8_t *value, size_t size)
{
   uint8_t *out = malloc(size);
   assert(out);
   assert(blob_get(key, CACHE_KEY_SIZE, out, size) == (long)size);
   assert(memcmp(out, value, size) == 0);
   free(out);
}

int main(int argc, char **argv)
{
   const char *const dir = argc > 1 ? argv[1] : ".";
   snprintf(store_path, sizeof(store_path), "%s/cache_commit_store.bin", dir);
   snprintf(index_path, sizeof(index_path), "%s/cache_commit_store.idx", dir);

   uint8_t key_a[CACHE_KEY_SIZE] = {0xa1}, key_b[CACHE_KEY_SIZE] = {0xb2},
           key_c[CACHE_KEY_SIZE] = {0xc3}, key_x[CACHE_KEY_SIZE] = {0xd4};
   const size_t small = 8192;
   uint8_t *data = malloc(DISK_CACHE_HORIZON_MAX_BLOB);
   assert(data);
   memset(data, 0x6b, DISK_CACHE_HORIZON_MAX_BLOB);
   const uint64_t small_record = sizeof(struct store_record) + small;

   /* Batched policy: a process that dies after appending leaves those records
    * behind the published log end. The next process must ignore them, reuse
    * the space and stay consistent. */
   create_store(4 * DISK_CACHE_HORIZON_MAX_BLOB, false);
   blob_put(key_a, CACHE_KEY_SIZE, data, small);
   blob_put(key_b, CACHE_KEY_SIZE, data, small);
   assert(store.commit_pending_B == 2 * small_record);
   assert(header_used() == sizeof(struct store_header)); /* nothing published */
   crash();

   reopen(false);
   assert(store.used == sizeof(struct store_header));
   assert(store.indexed == sizeof(struct store_header));
   assert(_mesa_hash_table_num_entries(&table) == 0);

   /* The next append overwrites the orphaned records instead of growing the
    * store; the orphaned keys are gone, the new one is usable. */
   blob_put(key_a, CACHE_KEY_SIZE, data, small);
   assert(entry_for(key_a) != NULL &&
          entry_for(key_a)->offset ==
             sizeof(struct store_header) + sizeof(struct store_record));
   assert(entry_for(key_b) == NULL);
   assert(store.used == sizeof(struct store_header) + small_record);
   verify_blob(key_a, data, small);
   assert(blob_get(key_b, CACHE_KEY_SIZE, data, small) == 0);

   /* A clean close publishes what is pending, and the index carries it. The
    * store lock is held for both, as disk_cache_horizon_fini() does. */
   simple_mtx_lock(&store.mtx);
   assert(store_commit());
   index_write();
   simple_mtx_unlock(&store.mtx);
   assert(header_used() == sizeof(struct store_header) + small_record);
   crash();

   reopen(false);
   assert(store.used == sizeof(struct store_header) + small_record);
   assert(_mesa_hash_table_num_entries(&table) == 1);
   counters();
   verify_blob(key_a, data, small);
   assert(blob_get(key_b, CACHE_KEY_SIZE, data, small) == 0);

   /* Crossing the publication interval publishes by itself: the recovered log
    * end covers every blob appended before that point. */
   for (unsigned i = 0; i < 4; i++) {
      uint8_t k[CACHE_KEY_SIZE] = {0};
      k[0] = 0x41 + i;
      blob_put(k, CACHE_KEY_SIZE, data, small);
   }
   blob_put(key_c, CACHE_KEY_SIZE, data, DISK_CACHE_HORIZON_MAX_BLOB);
   assert(store.commit_pending_B == 0);
   const uint64_t published = header_used();
   assert(published == store.used);
   blob_put(key_x, CACHE_KEY_SIZE, data, small); /* pending again */
   const uint64_t tail = store.used;
   assert(tail > published && header_used() == published);
   crash();

   reopen(false);
   assert(store.used == published);
   assert(_mesa_hash_table_num_entries(&table) == 6);
   counters();
   verify_blob(key_c, data, DISK_CACHE_HORIZON_MAX_BLOB);
   assert(blob_get(key_x, CACHE_KEY_SIZE, data, small) == 0);

   /* Per-blob policy: the same crash keeps every appended blob, because each
    * one was published with its own record. */
   create_store(4 * DISK_CACHE_HORIZON_MAX_BLOB, true);
   blob_put(key_a, CACHE_KEY_SIZE, data, small);
   blob_put(key_b, CACHE_KEY_SIZE, data, small);
   assert(store.commit_pending_B == 0);
   assert(header_used() == sizeof(struct store_header) + 2 * small_record);
   crash();

   reopen(true);
   assert(store.used == sizeof(struct store_header) + 2 * small_record);
   assert(_mesa_hash_table_num_entries(&table) == 2);
   counters();
   verify_blob(key_a, data, small);
   verify_blob(key_b, data, small);

   /* An index that claims more than the published header is rejected and the
    * store is rescanned instead of trusted. */
   FILE *f = fopen(store_path, "r+b");
   assert(f != NULL);
   struct store_header hdr;
   assert(fread(&hdr, sizeof(hdr), 1, f) == 1);
   const uint64_t shrunk = hdr.used - small_record;
   hdr.used = shrunk;
   assert(fseek(f, 0, SEEK_SET) == 0);
   assert(fwrite(&hdr, sizeof(hdr), 1, f) == 1);
   fclose(f);

   crash();
   reopen(true);
   assert(store.used == shrunk);
   assert(_mesa_hash_table_num_entries(&table) == 1);
   counters();
   verify_blob(key_a, data, small);

   fclose(store.file);
   store.file = NULL;
   table_clear();
   free(data);
   remove(store_path);
   remove(index_path);
   puts("PASS: batched publications lose only unpublished appends across an abrupt exit; "
        "per-blob publications lose none; index/header disagreement rescans");
   return 0;
}
