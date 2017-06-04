/* dzl-fuzzy-index-builder.c
 *
 * Copyright (C) 2016 Christian Hergert <christian@hergert.me>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define G_LOG_DOMAIN    "dzl-fuzzy-index-builder"
#define MAX_KEY_ENTRIES (0x00FFFFFF)

#include <stdlib.h>
#include <string.h>

#include "fuzzy/dzl-fuzzy-index-builder.h"
#include "fuzzy/dzl-fuzzy-util.h"

struct _DzlFuzzyIndexBuilder
{
  GObject       object;

  guint         case_sensitive : 1;

  /*
   * This hash table contains a mapping of GVariants so that we
   * deduplicate insertions of the same document. This helps when
   * we have indexes that contain multiple strings to the same
   * piece of data.
   */
  GHashTable *documents_hash;

  /*
   * This array contains a pointer to the individual GVariants
   * while building the index. When writing the index to disk,
   * we create a fixed array from this array of varians.
   */
  GPtrArray *documents;

  /*
   * Since we will need to keep a copy of a lot of strings, we
   * use a GString chunk to reduce the presure on the allocator.
   * It can certainly leave some gaps that are unused in the
   * sequence of pages, but it is generally better than using
   * a GByteArray or some other pow^2 growing array.
   */
  GStringChunk *strings;

  /*
   * This maps a pointer to a string that is found in the strings
   * string chunk to a key id (stored as a pointer). The input
   * string must exist within strings as we use a direct hash from
   * the input pointer to map to the string to save on the cost
   * of key equality checks.
   */
  GHashTable *key_ids;

  /*
   * An array of keys where the index of the key is the "key_id" used
   * in other structures. The pointer points to a key within the
   * strings GStringChunk.
   */
  GPtrArray *keys;

  /*
   * This array maps our document id to a key id. When building the
   * search index we use this to disambiguate between multiple
   * documents pointing to the same document.
   */
  GArray *kv_pairs;

  /*
   * Metadata for the search index, which is stored as the "metadata"
   * key in the final search index. You can use fuzzy_index_get_metadata()
   * to retrieve values stored here.
   *
   * This might be useful to store things like the mtime of the data
   * you are indexes so that you know if you need to reindex. You might
   * also store the version of your indexer here so that when you update
   * your indexer code, you can force a rebuild of the index.
   */
  GHashTable *metadata;
};

typedef struct
{
  /* The position within the keys array of the key. */
  guint key_id;

  /* The position within the documents array of the document */
  guint document_id;
} KVPair;

typedef struct
{
  /*
   * The character position within the string in terms of unicode
   * characters, not byte-position.
   */
  guint position;

  /* The document id (which is the hash of the document. */
  guint lookaside_id;
} IndexItem;

G_DEFINE_TYPE (DzlFuzzyIndexBuilder, dzl_fuzzy_index_builder, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_CASE_SENSITIVE,
  N_PROPS
};

static GParamSpec *properties [N_PROPS];

static guint
mask_priority (guint key_id)
{
  return key_id & 0x00FFFFFF;
}

static void
dzl_fuzzy_index_builder_finalize (GObject *object)
{
  DzlFuzzyIndexBuilder *self = (DzlFuzzyIndexBuilder *)object;

  g_clear_pointer (&self->documents_hash, g_hash_table_unref);
  g_clear_pointer (&self->documents, g_ptr_array_unref);
  g_clear_pointer (&self->strings, g_string_chunk_free);
  g_clear_pointer (&self->kv_pairs, g_array_unref);
  g_clear_pointer (&self->metadata, g_hash_table_unref);
  g_clear_pointer (&self->key_ids, g_hash_table_unref);

  G_OBJECT_CLASS (dzl_fuzzy_index_builder_parent_class)->finalize (object);
}

static void
dzl_fuzzy_index_builder_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  DzlFuzzyIndexBuilder *self = DZL_FUZZY_INDEX_BUILDER (object);

  switch (prop_id)
    {
    case PROP_CASE_SENSITIVE:
      g_value_set_boolean (value, dzl_fuzzy_index_builder_get_case_sensitive (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
dzl_fuzzy_index_builder_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  DzlFuzzyIndexBuilder *self = DZL_FUZZY_INDEX_BUILDER (object);

  switch (prop_id)
    {
    case PROP_CASE_SENSITIVE:
      dzl_fuzzy_index_builder_set_case_sensitive (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
dzl_fuzzy_index_builder_class_init (DzlFuzzyIndexBuilderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = dzl_fuzzy_index_builder_finalize;
  object_class->get_property = dzl_fuzzy_index_builder_get_property;
  object_class->set_property = dzl_fuzzy_index_builder_set_property;

  properties [PROP_CASE_SENSITIVE] =
    g_param_spec_boolean ("case-sensitive",
                          "Case Sensitive",
                          "Case Sensitive",
                          FALSE,
                          (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
dzl_fuzzy_index_builder_init (DzlFuzzyIndexBuilder *self)
{
  self->documents = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
  self->documents_hash = g_hash_table_new (fuzzy_g_variant_hash, g_variant_equal);
  self->kv_pairs = g_array_new (FALSE, FALSE, sizeof (KVPair));
  self->strings = g_string_chunk_new (4096);
  self->key_ids = g_hash_table_new (NULL, NULL);
  self->keys = g_ptr_array_new ();
}

DzlFuzzyIndexBuilder *
dzl_fuzzy_index_builder_new (void)
{
  return g_object_new (DZL_TYPE_FUZZY_INDEX_BUILDER, NULL);
}

/**
 * dzl_fuzzy_index_builder_insert:
 * @self: A #DzlFuzzyIndexBuilder
 * @key: The UTF-8 encoded key for the document
 * @document: The document to store
 * @priority: An optional priority for the keyword.
 *
 * Inserts @document into the index using @key as the lookup key.
 *
 * If a matching document (checked by hashing @document) has already
 * been inserted, only a single instance of the document will be stored.
 *
 * If @document is floating, it's floating reference will be sunk using
 * g_variant_ref_sink().
 *
 * @priority may be used to group results by priority. Priority must be
 * less than 256.
 *
 * Returns: The document id registered for @document.
 */
guint64
dzl_fuzzy_index_builder_insert (DzlFuzzyIndexBuilder *self,
                                const gchar          *key,
                                GVariant             *document,
                                guint                 priority)
{
  GVariant *real_document = NULL;
  gpointer document_id = NULL;
  gpointer key_id = NULL;
  KVPair pair;

  g_return_val_if_fail (DZL_IS_FUZZY_INDEX_BUILDER (self), 0L);
  g_return_val_if_fail (key != NULL, 0L);
  g_return_val_if_fail (document != NULL, 0L);
  g_return_val_if_fail (priority <= 0xFF, 0L);

  if (self->keys->len > MAX_KEY_ENTRIES)
    {
      g_warning ("Index is full, cannot add more entries");
      return 0L;
    }

  key = g_string_chunk_insert_const (self->strings, key);

  if (!g_hash_table_lookup_extended (self->documents_hash,
                                     document,
                                     (gpointer *)&real_document,
                                     &document_id))
    {
      document_id = GUINT_TO_POINTER (self->documents->len);
      real_document = g_variant_ref_sink (document);
      g_ptr_array_add (self->documents, real_document);
      g_hash_table_insert (self->documents_hash, real_document, document_id);
    }

  if (!g_hash_table_lookup_extended (self->key_ids, key, NULL, &key_id))
    {
      key_id = GUINT_TO_POINTER (self->keys->len | ((priority & 0xFF) << 24));
      g_ptr_array_add (self->keys, (gchar *)key);
      g_hash_table_insert (self->key_ids, (gpointer)key, key_id);
    }

  pair.key_id = GPOINTER_TO_UINT (key_id);
  pair.document_id = GPOINTER_TO_UINT (document_id);

  g_array_append_val (self->kv_pairs, pair);

  return pair.document_id;
}

static gint
pos_doc_pair_compare (gconstpointer a,
                      gconstpointer b)
{
  const IndexItem *paira = a;
  const IndexItem *pairb = b;
  gint ret;

  ret = paira->lookaside_id - pairb->lookaside_id;

  if (ret == 0)
    ret = paira->position - pairb->position;

  return ret;
}

static GVariant *
dzl_fuzzy_index_builder_build_keys (DzlFuzzyIndexBuilder *self)
{
  g_assert (DZL_IS_FUZZY_INDEX_BUILDER (self));

  return g_variant_new_strv ((const gchar * const *)self->keys->pdata,
                             self->keys->len);
}

static GVariant *
dzl_fuzzy_index_builder_build_lookaside (DzlFuzzyIndexBuilder *self)
{
  g_assert (DZL_IS_FUZZY_INDEX_BUILDER (self));

  return g_variant_new_fixed_array ((const GVariantType *)"(uu)",
                                    self->kv_pairs->data,
                                    self->kv_pairs->len,
                                    sizeof (KVPair));
}

static GVariant *
dzl_fuzzy_index_builder_build_index (DzlFuzzyIndexBuilder *self)
{
  g_autoptr(GPtrArray) ar = NULL;
  g_autoptr(GHashTable) rows = NULL;
  GVariantDict dict;
  GHashTableIter iter;
  gpointer keyptr;
  GArray *row;
  guint i;

  g_assert (DZL_IS_FUZZY_INDEX_BUILDER (self));

  ar = g_ptr_array_new_with_free_func ((GDestroyNotify)g_array_unref);
  rows = g_hash_table_new (NULL, NULL);

  for (i = 0; i < self->kv_pairs->len; i++)
    {
      g_autofree gchar *lower = NULL;
      KVPair *kvpair = &g_array_index (self->kv_pairs, KVPair, i);
      IndexItem item = { 0, i };
      const gchar *key;
      const gchar *tmp;
      guint position = 0;

      key = g_ptr_array_index (self->keys, mask_priority (kvpair->key_id));

      if (!self->case_sensitive)
        key = lower = g_utf8_casefold (key, -1);

      for (tmp = key; *tmp; tmp = g_utf8_next_char (tmp))
        {
          gunichar ch = g_utf8_get_char (tmp);

          row = g_hash_table_lookup (rows, GUINT_TO_POINTER (ch));

          if G_UNLIKELY (row == NULL)
            {
              row = g_array_new (FALSE, FALSE, sizeof (IndexItem));
              g_hash_table_insert (rows, GUINT_TO_POINTER (ch), row);
            }

          item.position = position++;
          g_array_append_val (row, item);
        }
    }

  g_variant_dict_init (&dict, NULL);

  g_hash_table_iter_init (&iter, rows);

  while (g_hash_table_iter_next (&iter, &keyptr, (gpointer *)&row))
    {
      gchar key[8];
      GVariant *variant;
      gunichar ch = GPOINTER_TO_UINT (keyptr);

      key [g_unichar_to_utf8 (ch, key)] = 0;

      g_array_sort (row, pos_doc_pair_compare);

      variant = g_variant_new_fixed_array ((const GVariantType *)"(uu)",
                                           row->data,
                                           row->len,
                                           sizeof (IndexItem));
      g_variant_dict_insert_value (&dict, key, variant);
    }

  return g_variant_dict_end (&dict);
}

static GVariant *
dzl_fuzzy_index_builder_build_metadata (DzlFuzzyIndexBuilder *self)
{
  GVariantDict dict;
  GHashTableIter iter;

  g_assert (DZL_IS_FUZZY_INDEX_BUILDER (self));

  g_variant_dict_init (&dict, NULL);

  if (self->metadata != NULL)
    {
      const gchar *key;
      GVariant *value;

      g_hash_table_iter_init (&iter, self->metadata);
      while (g_hash_table_iter_next (&iter, (gpointer *)&key, (gpointer *)&value))
        g_variant_dict_insert_value (&dict, key, value);
    }

  g_variant_dict_insert (&dict, "case-sensitive", "b", self->case_sensitive);

  return g_variant_dict_end (&dict);
}

static void
dzl_fuzzy_index_builder_write_worker (GTask        *task,
                                      gpointer      source_object,
                                      gpointer      task_data,
                                      GCancellable *cancellable)
{
  DzlFuzzyIndexBuilder *self = source_object;
  g_autoptr(GVariant) variant = NULL;
  g_autoptr(GVariant) documents = NULL;
  g_autoptr(GPtrArray) array_of_keys = NULL;
  GVariantDict dict;
  GFile *file = task_data;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));
  g_assert (DZL_IS_FUZZY_INDEX_BUILDER (self));
  g_assert (G_IS_FILE (file));
  g_assert (!cancellable || G_IS_CANCELLABLE (cancellable));

  g_variant_dict_init (&dict, NULL);

  /* Set our version number for the document */
  g_variant_dict_insert (&dict, "version", "i", 1);

  /* Build our dicitionary of metadata */
  g_variant_dict_insert_value (&dict,
                               "metadata",
                               dzl_fuzzy_index_builder_build_metadata (self));

  /* Keys is an array of string keys where the index is the "key_id" */
  g_variant_dict_insert_value (&dict,
                               "keys",
                               dzl_fuzzy_index_builder_build_keys (self));

  /* The lookaside is a mapping of kvpair to the repsective keys and
   * documents. This allows the tables to use the kvpair id as the value
   * in the index so we can have both document deduplication as well as
   * the ability to disambiguate the keys which point to the same
   * document. The contents are "a{uu}".
   */
  g_variant_dict_insert_value (&dict,
                               "lookaside",
                               dzl_fuzzy_index_builder_build_lookaside (self));

  /* Build our dicitionary of character → [(pos,lookaside_id),..] tuples.
   * The position is the utf8 character position within the string.
   * The lookaside_id is the index within the lookaside buffer to locate
   * the document_id or key_id.
   */
  g_variant_dict_insert_value (&dict,
                               "tables",
                               dzl_fuzzy_index_builder_build_index (self));

  /*
   * The documents are stored as an array where the document identifier is
   * their index position. We then use a lookaside buffer to map the insertion
   * id to the document id. Otherwise, we can't disambiguate between two
   * keys that insert the same document (as we deduplicate documents inserted
   * into the index).
   */
  documents = g_variant_new_array (NULL,
                                   (GVariant * const *)self->documents->pdata,
                                   self->documents->len);
  g_variant_dict_insert_value (&dict, "documents", g_variant_ref_sink (documents));

  /* Now write the variant to disk */
  variant = g_variant_ref_sink (g_variant_dict_end (&dict));
  if (!g_file_replace_contents (file,
                                g_variant_get_data (variant),
                                g_variant_get_size (variant),
                                NULL,
                                FALSE,
                                G_FILE_CREATE_NONE,
                                NULL,
                                cancellable,
                                &error))
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);
}

/**
 * dzl_fuzzy_index_builder_write_async:
 * @self: A #DzlFuzzyIndexBuilder
 * @file: A #GFile to write the index to
 * @io_priority: The priority for IO operations
 * @cancellable: (nullable): An optional #GCancellable or %NULL
 * @callback: A callback for completion or %NULL
 * @user_data: User data for @callback
 *
 * Builds and writes the index to @file. The file format is a
 * GVariant on disk and can be loaded and searched using
 * #FuzzyIndex.
 */
void
dzl_fuzzy_index_builder_write_async (DzlFuzzyIndexBuilder *self,
                                     GFile                *file,
                                     gint                  io_priority,
                                     GCancellable         *cancellable,
                                     GAsyncReadyCallback   callback,
                                     gpointer              user_data)
{
  g_autoptr(GTask) task = NULL;

  g_return_if_fail (DZL_IS_FUZZY_INDEX_BUILDER (self));
  g_return_if_fail (G_IS_FILE (file));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, dzl_fuzzy_index_builder_write_async);
  g_task_set_priority (task, io_priority);
  g_task_set_task_data (task, g_object_ref (file), g_object_unref);
  g_task_run_in_thread (task, dzl_fuzzy_index_builder_write_worker);
}

gboolean
dzl_fuzzy_index_builder_write_finish (DzlFuzzyIndexBuilder  *self,
                                      GAsyncResult          *result,
                                      GError               **error)
{
  g_return_val_if_fail (DZL_IS_FUZZY_INDEX_BUILDER (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

gboolean
dzl_fuzzy_index_builder_write (DzlFuzzyIndexBuilder  *self,
                               GFile                 *file,
                               gint                   io_priority,
                               GCancellable          *cancellable,
                               GError               **error)
{
  g_autoptr(GTask) task = NULL;

  g_return_val_if_fail (DZL_IS_FUZZY_INDEX_BUILDER (self), FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);
  g_return_val_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable), FALSE);

  task = g_task_new (self, cancellable, NULL, NULL);
  g_task_set_source_tag (task, dzl_fuzzy_index_builder_write);
  g_task_set_priority (task, io_priority);
  g_task_set_task_data (task, g_object_ref (file), g_object_unref);

  dzl_fuzzy_index_builder_write_worker (task, self, file, cancellable);

  return g_task_propagate_boolean (task, error);
}

/**
 * dzl_fuzzy_index_builder_get_document:
 *
 * Returns the document that was inserted in a previous call to
 * dzl_fuzzy_index_builder_insert().
 *
 * Returns: (transfer none): A #GVariant
 */
const GVariant *
dzl_fuzzy_index_builder_get_document (DzlFuzzyIndexBuilder *self,
                                      guint64               document_id)
{
  g_return_val_if_fail (DZL_IS_FUZZY_INDEX_BUILDER (self), NULL);
  g_return_val_if_fail ((guint)document_id < self->documents->len, NULL);

  return g_ptr_array_index (self->documents, (guint)document_id);
}

void
dzl_fuzzy_index_builder_set_metadata (DzlFuzzyIndexBuilder *self,
                                      const gchar          *key,
                                      GVariant             *value)
{
  g_return_if_fail (DZL_IS_FUZZY_INDEX_BUILDER (self));
  g_return_if_fail (key != NULL);

  if (self->metadata == NULL)
    self->metadata = g_hash_table_new_full (g_str_hash,
                                            g_str_equal,
                                            g_free,
                                            (GDestroyNotify)g_variant_unref);

  if (value != NULL)
    g_hash_table_insert (self->metadata,
                         g_strdup (key),
                         g_variant_ref_sink (value));
  else
    g_hash_table_remove (self->metadata, key);
}

void
dzl_fuzzy_index_builder_set_metadata_string (DzlFuzzyIndexBuilder *self,
                                             const gchar          *key,
                                             const gchar          *value)
{
  g_return_if_fail (DZL_IS_FUZZY_INDEX_BUILDER (self));
  g_return_if_fail (key != NULL);
  g_return_if_fail (value != NULL);

  dzl_fuzzy_index_builder_set_metadata (self, key, g_variant_new_string (value));
}

void
dzl_fuzzy_index_builder_set_metadata_uint32 (DzlFuzzyIndexBuilder *self,
                                             const gchar          *key,
                                             guint32               value)
{
  g_return_if_fail (DZL_IS_FUZZY_INDEX_BUILDER (self));
  g_return_if_fail (key != NULL);

  dzl_fuzzy_index_builder_set_metadata (self, key, g_variant_new_uint32 (value));
}

void
dzl_fuzzy_index_builder_set_metadata_uint64 (DzlFuzzyIndexBuilder *self,
                                             const gchar          *key,
                                             guint64               value)
{
  g_return_if_fail (DZL_IS_FUZZY_INDEX_BUILDER (self));
  g_return_if_fail (key != NULL);

  dzl_fuzzy_index_builder_set_metadata (self, key, g_variant_new_uint64 (value));
}

gboolean
dzl_fuzzy_index_builder_get_case_sensitive (DzlFuzzyIndexBuilder *self)
{
  g_return_val_if_fail (DZL_IS_FUZZY_INDEX_BUILDER (self), FALSE);

  return self->case_sensitive;
}

void
dzl_fuzzy_index_builder_set_case_sensitive (DzlFuzzyIndexBuilder *self,
                                            gboolean              case_sensitive)
{
  g_return_if_fail (DZL_IS_FUZZY_INDEX_BUILDER (self));

  case_sensitive = !!case_sensitive;

  if (self->case_sensitive != case_sensitive)
    {
      self->case_sensitive = case_sensitive;
      g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_CASE_SENSITIVE]);
    }
}
