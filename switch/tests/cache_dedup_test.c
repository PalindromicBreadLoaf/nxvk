/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real production put/get/publish/compare bodies and a real temporary FILE.
 * The index, mutex and CRC are fixtures; a constant CRC deliberately forces
 * collisions to prove that deduplication requires byte equality as well.
 * The whole scenario runs twice: once with header publication batched by
 * STORE_COMMIT_INTERVAL_B, once with the per-blob policy restored. */
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
#include "cache_defines.inc"
#include "cache_structs.inc"
struct hash_entry { void *data; };
struct hash_table { bool present; struct store_entry entry; struct hash_entry he; };
static struct {
   unsigned mtx;
   FILE *file;
   struct hash_table *index;
   uint64_t used, max_size, id;
   uint64_t commit_pending_B;
   bool full;
   bool commit_per_blob;
} store;
static struct store_stats stats;
static struct hash_table table;
static unsigned write_calls, read_calls, flush_calls, crc_calls, full_messages;
static unsigned warn_messages;
static size_t write_bytes, read_bytes;
static unsigned fail_write, fail_read, fail_seek;
static struct hash_entry *_mesa_hash_table_search(struct hash_table *t, const void *key)
{
   assert(store.mtx == 1);
   if (!t->present || memcmp(t->entry.key, key, CACHE_KEY_SIZE)) return NULL;
   t->he.data = &t->entry;
   return &t->he;
}
static void index_put(const uint8_t *key, uint64_t offset, uint32_t size, uint32_t crc)
{
   assert(store.mtx == 1);
   memcpy(table.entry.key, key, CACHE_KEY_SIZE);
   table.entry.offset = offset; table.entry.size = size; table.entry.crc = crc;
   table.present = true;
}
static void simple_mtx_lock(unsigned *m) { assert(*m == 0); *m = 1; }
static void simple_mtx_unlock(unsigned *m) { assert(*m == 1); *m = 0; }
static uint32_t util_hash_crc32(const void *value, size_t size) { crc_calls++; return 0x12345678; }
#define mesa_logi(...) ((void)++full_messages)
#define mesa_logw(...) ((void)++warn_messages)
static size_t observed_write(const void *p, size_t size, size_t n, FILE *f)
{
   write_calls++;
   if (fail_write && write_calls == fail_write) return 0;
   size_t count = fwrite(p, size, n, f);
   write_bytes += size * count;
   return count;
}
static size_t observed_read(void *p, size_t size, size_t n, FILE *f)
{
   read_calls++;
   if (fail_read) { fail_read--; return 0; }
   size_t count = fread(p, size, n, f);
   read_bytes += size * count;
   return count;
}
static int observed_flush(FILE *f) { flush_calls++; return fflush(f); }
static int observed_seek(FILE *f, long off, int whence)
{
   if (fail_seek) { fail_seek--; return -1; }
   return fseek(f, off, whence);
}
#define fwrite observed_write
#define fread observed_read
#define fflush observed_flush
#define fseek observed_seek
#include "cache_functions.inc"
#undef fwrite
#undef fread
#undef fflush
#undef fseek

/* Expected store I/O for one appended blob, per publication policy: the record
 * and its payload are always written and flushed, the header only in per-blob
 * mode (or when a batched publication becomes due). */
#define PUT_WRITES()       (store.commit_per_blob ? 3u : 2u)
#define PUT_FLUSHES()      (store.commit_per_blob ? 2u : 1u)
#define PUT_WRITE_BYTES(n) (sizeof(struct store_record) + (size_t)(n) + \
                            (store.commit_per_blob ? sizeof(struct store_header) : 0))

static struct store_stats stats_before;
#define STATS(field) (stats.field - stats_before.field)

static void counters(void)
{
   write_calls = read_calls = flush_calls = crc_calls = full_messages = 0;
   warn_messages = 0;
   write_bytes = read_bytes = 0;
   fail_write = fail_read = fail_seek = 0;
   stats_before = stats;
}
static void reset(uint64_t capacity, bool commit_per_blob)
{
   if (store.file) fclose(store.file);
   memset(&store, 0, sizeof(store)); memset(&table, 0, sizeof(table));
   store.file = tmpfile(); assert(store.file);
   store.index = &table; store.id = 17;
   store.used = sizeof(struct store_header); store.max_size = capacity;
   store.commit_per_blob = commit_per_blob;
   assert(store_commit());
   stats = (struct store_stats) { 0 };
   counters();
}
static void verify(const uint8_t *key, const uint8_t *value, size_t size)
{
   uint8_t *out = malloc(size); assert(out);
   assert(blob_get(key, CACHE_KEY_SIZE, out, size) == (long)size);
   assert(memcmp(out, value, size) == 0);
   assert(store.mtx == 0);
   free(out);
}

static void run_scenario(bool commit_per_blob)
{
   const char *const policy = commit_per_blob ? "per-blob" : "batched";
   uint8_t key[CACHE_KEY_SIZE] = {1}, other[CACHE_KEY_SIZE] = {2};
   uint8_t *data = malloc(DISK_CACHE_HORIZON_MAX_BLOB + 1); assert(data);
   memset(data, 0x5a, DISK_CACHE_HORIZON_MAX_BLOB + 1);
   reset(4 * DISK_CACHE_HORIZON_MAX_BLOB, commit_per_blob);
   blob_put(key, CACHE_KEY_SIZE - 1, data, 1);
   blob_put(key, CACHE_KEY_SIZE, data, 0);
   blob_put(key, CACHE_KEY_SIZE, data, -1);
   blob_put(key, CACHE_KEY_SIZE, data, DISK_CACHE_HORIZON_MAX_BLOB + 1);
   assert(write_calls == 0 && crc_calls == 0 && store.mtx == 0);
   assert(STATS(puts) == 0 && STATS(appends) == 0);

   /* First write appends and flushes the record; the header publication
    * follows the configured policy. */
   blob_put(key, CACHE_KEY_SIZE, data, 8193);
   assert(write_calls == PUT_WRITES() && flush_calls == PUT_FLUSHES() && read_calls == 0);
   assert(crc_calls == 1 && write_bytes == PUT_WRITE_BYTES(8193));
   assert(STATS(puts) == 1 && STATS(appends) == 1 && STATS(duplicates) == 0);
   assert(STATS(commits) == (commit_per_blob ? 1 : 0));
   assert(store.commit_pending_B == (commit_per_blob ? 0 : sizeof(struct store_record) + 8193));
   uint64_t used = store.used, offset = table.entry.offset;
   counters();
   blob_put(key, CACHE_KEY_SIZE, data, 8193);
   assert(store.used == used && table.entry.offset == offset);
   assert(write_calls == 0 && flush_calls == 0 && read_bytes == 8193 && crc_calls == 1);
   assert(STATS(puts) == 1 && STATS(appends) == 0 && STATS(duplicates) == 1);
   assert(STATS(commits) == 0 && store.commit_pending_B == (commit_per_blob ? 0 : sizeof(struct store_record) + 8193));
   verify(key, data, 8193);

   /* Equal key, size and CRC, but a difference after two chunks: must replace. */
   data[8192] ^= 1;
   counters();
   blob_put(key, CACHE_KEY_SIZE, data, 8193);
   assert(store.used == used + sizeof(struct store_record) + 8193);
   assert(write_calls == PUT_WRITES() && flush_calls == PUT_FLUSHES() && crc_calls == 1);
   assert(STATS(appends) == 1 && STATS(duplicates) == 0);
   verify(key, data, 8193);

   /* Different sizes still replace normally. */
   used = store.used; counters();
   blob_put(key, CACHE_KEY_SIZE, data, 8192);
   assert(store.used == used + sizeof(struct store_record) + 8192 && read_calls == 0);
   verify(key, data, 8192);

   /* On-disk corruption must not be hidden by an otherwise matching index. */
   assert(fseek(store.file, table.entry.offset + 17, SEEK_SET) == 0);
   assert(fputc(0x33, store.file) == 0x33 && fflush(store.file) == 0);
   used = store.used; counters();
   blob_put(key, CACHE_KEY_SIZE, data, 8192);
   assert(store.used > used && write_calls == PUT_WRITES());
   verify(key, data, 8192);

   /* A corrupt stored CRC is repaired without trusting the indexed payload. */
   table.entry.crc ^= 1;
   used = store.used; counters();
   blob_put(key, CACHE_KEY_SIZE, data, 8192);
   assert(store.used > used && read_calls == 0 && crc_calls == 1);
   verify(key, data, 8192);

   /* A failed read/seek falls back to append, never reports a duplicate. */
   used = store.used; counters(); fail_read = 1;
   blob_put(key, CACHE_KEY_SIZE, data, 8192);
   assert(store.used > used && write_calls == PUT_WRITES());
   verify(key, data, 8192);
   used = store.used; counters(); fail_seek = 1;
   blob_put(key, CACHE_KEY_SIZE, data, 8192);
   assert(store.used > used && write_calls == PUT_WRITES());
   verify(key, data, 8192);

   /* Overflowed offsets are rejected without reading outside committed data. */
   table.entry.offset = UINT64_MAX;
   used = store.used; counters();
   blob_put(key, CACHE_KEY_SIZE, data, 8192);
   assert(store.used > used && read_calls == 0);
   verify(key, data, 8192);

   /* A failed record write never advances the log or the index, in either
    * policy. In per-blob mode the failed header publication rolls the append
    * back instead of keeping an unpublished record. */
   used = store.used; offset = table.entry.offset;
   data[0] = 0x72; counters();
   fail_write = commit_per_blob ? 3 : 2;
   blob_put(key, CACHE_KEY_SIZE, data, 8192);
   assert(store.used == used && table.entry.offset == offset && store.mtx == 0);
   assert(STATS(appends) == 0 && STATS(commits) == 0 && warn_messages == 0);
   counters(); data[0] = 0x5a;
   verify(key, data, 8192);

   /* Batched publication: a due header write that fails keeps the appended
    * record and its index entry, warns once, and retries on the next append. */
   if (!commit_per_blob) {
      used = store.used; counters();
      data[0] = 0x11; fail_write = 3;
      blob_put(key, CACHE_KEY_SIZE, data, DISK_CACHE_HORIZON_MAX_BLOB);
      assert(store.used > used && table.entry.size == DISK_CACHE_HORIZON_MAX_BLOB);
      /* Record and payload writes, then the failing header write; a failed
       * header write returns before its flush. */
      assert(write_calls == 3 && flush_calls == 1);
      assert(store.commit_pending_B >= STORE_COMMIT_INTERVAL_B);
      assert(STATS(appends) == 1 && STATS(commits) == 0 && warn_messages == 1);
      verify(key, data, DISK_CACHE_HORIZON_MAX_BLOB);

      counters();
      data[0] = 0x12;
      blob_put(other, CACHE_KEY_SIZE, data, 8192);
      assert(STATS(appends) == 1 && STATS(commits) == 1 && warn_messages == 0);
      assert(store.commit_pending_B == 0);
      assert(write_calls == 3 && flush_calls == 2);
      assert(write_bytes == sizeof(struct store_record) + 8192 + sizeof(struct store_header));
      verify(other, data, 8192);
   }

   /* Duplicates do not fill the store; genuinely new entries keep capacity checks. */
   reset(sizeof(struct store_header) + sizeof(struct store_record) + 64,
         commit_per_blob);
   blob_put(key, CACHE_KEY_SIZE, data, 64);
   used = store.used; counters();
   blob_put(key, CACHE_KEY_SIZE, data, 64);
   assert(store.used == used && write_calls == 0 && store.full);
   assert(read_calls == 0 && crc_calls == 0 && full_messages == 1);
   /* A full store rejects before the duplicate check, so it neither appends
    * nor re-reads the card. */
   assert(STATS(duplicates) == 0 && STATS(appends) == 0);
   blob_put(other, CACHE_KEY_SIZE, data, 64);
   assert(store.used == used && write_calls == 0 && store.full && full_messages == 1);

   /* Capacity arithmetic must not wrap on a corrupt/impossible used count. */
   store.used = UINT64_MAX - 1;
   store.max_size = UINT64_MAX;
   counters();
   blob_put(key, CACHE_KEY_SIZE, data, 64);
   assert(write_calls == 0 && read_calls == 0 && crc_calls == 0);

   /* A maximum-size blob makes a batched publication due by itself, and
    * duplicates of it still write nothing. */
   reset(4 * DISK_CACHE_HORIZON_MAX_BLOB, commit_per_blob);
   blob_put(key, CACHE_KEY_SIZE, data, DISK_CACHE_HORIZON_MAX_BLOB);
   assert(STATS(appends) == 1 && STATS(commits) == 1);
   assert(store.commit_pending_B == 0);
   used = store.used; counters();
   blob_put(key, CACHE_KEY_SIZE, data, DISK_CACHE_HORIZON_MAX_BLOB);
   assert(store.used == used && write_calls == 0 && read_bytes == DISK_CACHE_HORIZON_MAX_BLOB);
   assert(STATS(appends) == 0 && STATS(duplicates) == 1);
   verify(key, data, DISK_CACHE_HORIZON_MAX_BLOB);

   /* A small append leaves the batched policy pending; closing the store
    * publishes it and clears the pending count. */
   counters();
   blob_put(other, CACHE_KEY_SIZE, data, 8192);
   assert(STATS(appends) == 1);
   assert(store.commit_pending_B == (commit_per_blob ? 0 :
                                     sizeof(struct store_record) + 8192));
   counters();
   assert(store_commit());
   assert(write_calls == 1 && flush_calls == 1);
   assert(write_bytes == sizeof(struct store_header));
   assert(store.commit_pending_B == 0 && STATS(commits) == 1);

   fclose(store.file); store.file = NULL; counters();
   blob_put(key, CACHE_KEY_SIZE, data, 8);
   assert(write_calls == 0 && crc_calls == 0 && store.mtx == 0);
   free(data);
   printf("PASS: %s policy: dedupe, collisions, corruption, IO failures and "
          "commit batching behave as documented\n", policy);
}

int main(void)
{
   run_scenario(false /* batched publications */);
   run_scenario(true /* publication per blob */);
   puts("PASS: duplicate blob produces zero writes/flushes; collisions, corruption, IO failures, replacements and the batched publication policy remain safe");
   return 0;
}
