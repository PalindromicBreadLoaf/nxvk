/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real production put/get/commit/compare bodies and a real temporary FILE.
 * The index, mutex and CRC are fixtures; a constant CRC deliberately forces
 * collisions to prove that deduplication requires byte equality as well. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "util/disk_cache_horizon.h"
#define CACHE_KEY_SIZE 20
#define STORE_MAGIC 0x4353414d
#define STORE_VERSION 2
#define MIN2(a,b) ((a) < (b) ? (a) : (b))
#include "cache_structs.inc"
struct hash_entry { void *data; };
struct hash_table { bool present; struct store_entry entry; struct hash_entry he; };
static struct {
   unsigned mtx;
   FILE *file;
   struct hash_table *index;
   uint64_t used, max_size, id;
   bool full;
} store;
static struct hash_table table;
static unsigned write_calls, read_calls, flush_calls, crc_calls, full_messages;
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

static void counters(void)
{
   write_calls = read_calls = flush_calls = crc_calls = full_messages = 0;
   write_bytes = read_bytes = 0;
   fail_write = fail_read = fail_seek = 0;
}
static void reset(uint64_t capacity)
{
   if (store.file) fclose(store.file);
   memset(&store, 0, sizeof(store)); memset(&table, 0, sizeof(table));
   store.file = tmpfile(); assert(store.file);
   store.index = &table; store.id = 17;
   store.used = sizeof(struct store_header); store.max_size = capacity;
   assert(store_commit());
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

int main(void)
{
   uint8_t key[CACHE_KEY_SIZE] = {1}, other[CACHE_KEY_SIZE] = {2};
   uint8_t *data = malloc(DISK_CACHE_HORIZON_MAX_BLOB + 1); assert(data);
   memset(data, 0x5a, DISK_CACHE_HORIZON_MAX_BLOB + 1);
   reset(4 * DISK_CACHE_HORIZON_MAX_BLOB);
   blob_put(key, CACHE_KEY_SIZE - 1, data, 1);
   blob_put(key, CACHE_KEY_SIZE, data, 0);
   blob_put(key, CACHE_KEY_SIZE, data, -1);
   blob_put(key, CACHE_KEY_SIZE, data, DISK_CACHE_HORIZON_MAX_BLOB + 1);
   assert(write_calls == 0 && crc_calls == 0 && store.mtx == 0);

   /* First write retains the original payload/header publication protocol. */
   blob_put(key, CACHE_KEY_SIZE, data, 8193);
   assert(write_calls == 3 && flush_calls == 2 && read_calls == 0);
   assert(crc_calls == 1 && write_bytes == sizeof(struct store_record) + 8193 + sizeof(struct store_header));
   uint64_t used = store.used, offset = table.entry.offset;
   counters();
   blob_put(key, CACHE_KEY_SIZE, data, 8193);
   assert(store.used == used && table.entry.offset == offset);
   assert(write_calls == 0 && flush_calls == 0 && read_bytes == 8193 && crc_calls == 1);
   verify(key, data, 8193);

   /* Equal key, size and CRC, but a difference after two chunks: must replace. */
   data[8192] ^= 1;
   counters();
   blob_put(key, CACHE_KEY_SIZE, data, 8193);
   assert(store.used == used + sizeof(struct store_record) + 8193);
   assert(write_calls == 3 && flush_calls == 2 && crc_calls == 1);
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
   assert(store.used > used && write_calls == 3);
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
   assert(store.used > used && write_calls == 3);
   verify(key, data, 8192);
   used = store.used; counters(); fail_seek = 1;
   blob_put(key, CACHE_KEY_SIZE, data, 8192);
   assert(store.used > used && write_calls == 3);
   verify(key, data, 8192);

   /* Overflowed offsets are rejected without reading outside committed data. */
   table.entry.offset = UINT64_MAX;
   used = store.used; counters();
   blob_put(key, CACHE_KEY_SIZE, data, 8192);
   assert(store.used > used && read_calls == 0);
   verify(key, data, 8192);

   /* Failed header publication leaves the live index/used count unchanged. */
   used = store.used; offset = table.entry.offset;
   data[0] = 0x72; counters(); fail_write = 3;
   blob_put(key, CACHE_KEY_SIZE, data, 8192);
   assert(store.used == used && table.entry.offset == offset && store.mtx == 0);
   counters(); data[0] = 0x5a;
   verify(key, data, 8192);

   /* Duplicates do not fill the store; genuinely new entries keep capacity checks. */
   reset(sizeof(struct store_header) + sizeof(struct store_record) + 64);
   blob_put(key, CACHE_KEY_SIZE, data, 64);
   used = store.used; counters();
   blob_put(key, CACHE_KEY_SIZE, data, 64);
   assert(store.used == used && write_calls == 0 && store.full);
   assert(read_calls == 0 && crc_calls == 0 && full_messages == 1);
   blob_put(other, CACHE_KEY_SIZE, data, 64);
   assert(store.used == used && write_calls == 0 && store.full && full_messages == 1);

   /* Capacity arithmetic must not wrap on a corrupt/impossible used count. */
   store.used = UINT64_MAX - 1;
   store.max_size = UINT64_MAX;
   counters();
   blob_put(key, CACHE_KEY_SIZE, data, 64);
   assert(write_calls == 0 && read_calls == 0 && crc_calls == 0);

   reset(4 * DISK_CACHE_HORIZON_MAX_BLOB);
   blob_put(key, CACHE_KEY_SIZE, data, DISK_CACHE_HORIZON_MAX_BLOB);
   used = store.used; counters();
   blob_put(key, CACHE_KEY_SIZE, data, DISK_CACHE_HORIZON_MAX_BLOB);
   assert(store.used == used && write_calls == 0 && read_bytes == DISK_CACHE_HORIZON_MAX_BLOB);
   verify(key, data, DISK_CACHE_HORIZON_MAX_BLOB);

   fclose(store.file); store.file = NULL; counters();
   blob_put(key, CACHE_KEY_SIZE, data, 8);
   assert(write_calls == 0 && crc_calls == 0 && store.mtx == 0);
   free(data);
   puts("PASS: duplicate blob produces zero writes/flushes; collisions, corruption, IO failures and replacements remain safe");
   return 0;
}
