/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup edasset
 */

#include "ED_asset_indexer.h"

#include "DNA_asset_types.h"
#include "DNA_userdef_types.h"

#include "BLI_fileops.h"
#include "BLI_hash.hh"
#include "BLI_linklist.h"
#include "BLI_path_util.h"
#include "BLI_serialize.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"
#include "BLI_uuid.h"

#include "BKE_appdir.h"
#include "BKE_preferences.h"

#include "CLG_log.h"

#include <fstream>
#include <iomanip>
#include <optional>

static CLG_LogRef LOG = {"ed.asset"};

namespace blender::ed::asset {

class File {
 public:
  virtual const char *get_file_path() const = 0;

  const bool exists() const
  {
    return BLI_exists(get_file_path());
  }

  const size_t get_file_size() const
  {
    return BLI_file_size(get_file_path());
  }
};

class AssetFile : public File {
 public:
  StringRefNull file_name;

  AssetFile(StringRefNull file_name) : file_name(file_name)
  {
  }

  uint64_t hash() const
  {
    DefaultHash<StringRefNull> hasher;
    return hasher(file_name);
  }

  std::string get_name() const
  {
    char file_name[FILE_MAX];
    BLI_split_file_part(get_file_path(), file_name, sizeof(file_name));
    return std::string(file_name);
  }

  const char *get_file_path() const override
  {

    return file_name.c_str();
  }
};

constexpr StringRef ATTRIBUTE_VERSION = StringRef("version");
constexpr StringRef ATTRIBUTE_ENTRIES = StringRef("entries");
constexpr StringRef ATTRIBUTE_ENTRIES_NAME = StringRef("name");
constexpr StringRef ATTRIBUTE_ENTRIES_GROUP_NAME = StringRef("group_name");
constexpr StringRef ATTRIBUTE_ENTRIES_IDCODE = StringRef("idcode");
constexpr StringRef ATTRIBUTE_ENTRIES_CATALOG_ID = StringRef("catalog_id");
constexpr StringRef ATTRIBUTE_ENTRIES_CATALOG_NAME = StringRef("catalog_name");
constexpr StringRef ATTRIBUTE_ENTRIES_DESCRIPTION = StringRef("description");
constexpr StringRef ATTRIBUTE_ENTRIES_TAGS = StringRef("tags");

struct AssetEntryReader {
 private:
  blender::io::serialize::ObjectValue::Lookup lookup;

 public:
  AssetEntryReader(const blender::io::serialize::ObjectValue &entry)
      : lookup(entry.create_lookup())
  {
  }

  const int get_idcode() const
  {
    return lookup.lookup(ATTRIBUTE_ENTRIES_IDCODE)->as_int_value()->value();
  }

  const std::string &get_group_name() const
  {
    return lookup.lookup(ATTRIBUTE_ENTRIES_GROUP_NAME)->as_string_value()->string_value();
  }

  const std::string &get_name() const
  {
    return lookup.lookup(ATTRIBUTE_ENTRIES_NAME)->as_string_value()->string_value();
  }

  const bool has_description() const
  {
    return lookup.contains(ATTRIBUTE_ENTRIES_DESCRIPTION);
  }

  const std::string &get_description() const
  {
    return lookup.lookup(ATTRIBUTE_ENTRIES_DESCRIPTION)->as_string_value()->string_value();
  }

  const std::string &get_catalog_name() const
  {
    return lookup.lookup(ATTRIBUTE_ENTRIES_CATALOG_NAME)->as_string_value()->string_value();
  }

  const bUUID get_catalog_id() const
  {
    const std::string &catalog_id =
        lookup.lookup(ATTRIBUTE_ENTRIES_CATALOG_ID)->as_string_value()->string_value();
    bUUID catalog_uuid(catalog_id);
    return catalog_uuid;
  }

  const void get_tags(ListBase *tags) const
  {
    const blender::io::serialize::ObjectValue::LookupValue *value = lookup.lookup_ptr(
        ATTRIBUTE_ENTRIES_TAGS);
    if (value == nullptr) {
      return;
    }

    const blender::io::serialize::ArrayValue *array_value = (*value)->as_array_value();
    const blender::io::serialize::ArrayValue::Items &elements = array_value->elements();
    for (const blender::io::serialize::ArrayValue::Item &item : elements) {
      const std::string tag_name = item->as_string_value()->string_value();
      AssetTag *tag = static_cast<AssetTag *>(MEM_callocN(sizeof(AssetTag), __func__));
      BLI_strncpy(tag->name, tag_name.c_str(), sizeof(tag->name));
      BLI_addtail(tags, tag);
    }
  }
};

struct AssetEntryWriter {
 private:
  blender::io::serialize::ObjectValue::Items &attributes;

 public:
  AssetEntryWriter(blender::io::serialize::ObjectValue &entry) : attributes(entry.elements())
  {
  }

  void add_idcode(int idcode)
  {
    attributes.append_as(
        std::pair(ATTRIBUTE_ENTRIES_IDCODE, new blender::io::serialize::IntValue(idcode)));
  }
  void add_name(const StringRefNull name)
  {
    attributes.append_as(
        std::pair(ATTRIBUTE_ENTRIES_NAME, new blender::io::serialize::StringValue(name)));
  }
  void add_group_name(const StringRefNull group_name)
  {
    attributes.append_as(std::pair(ATTRIBUTE_ENTRIES_GROUP_NAME,
                                   new blender::io::serialize::StringValue(group_name)));
  }

  void add_catalog_id(const bUUID &catalog_id)
  {
    char catalog_id_str[UUID_STRING_LEN];
    BLI_uuid_format(catalog_id_str, catalog_id);
    attributes.append_as(std::pair(ATTRIBUTE_ENTRIES_CATALOG_ID,
                                   new blender::io::serialize::StringValue(catalog_id_str)));
  }

  void add_catalog_name(const StringRefNull catalog_name)
  {
    attributes.append_as(std::pair(ATTRIBUTE_ENTRIES_CATALOG_NAME,
                                   new blender::io::serialize::StringValue(catalog_name)));
  }

  void add_description(const StringRefNull description)
  {
    attributes.append_as(std::pair(ATTRIBUTE_ENTRIES_DESCRIPTION,
                                   new blender::io::serialize::StringValue(description)));
  }

  void add_tags(const ListBase /* AssetTag */ *asset_tags)
  {
    blender::io::serialize::ArrayValue *tags = new blender::io::serialize::ArrayValue();
    attributes.append_as(std::pair(ATTRIBUTE_ENTRIES_TAGS, tags));
    blender::io::serialize::ArrayValue::Items &tag_items = tags->elements();

    LISTBASE_FOREACH (AssetTag *, tag, asset_tags) {
      tag_items.append_as(new blender::io::serialize::StringValue(tag->name));
    }
  }
};

static void init_value_from_file_indexer_entry(AssetEntryWriter &result,
                                               const FileIndexerEntry *indexer_entry)
{
  const BLODataBlockInfo &datablock_info = indexer_entry->datablock_info;

  result.add_name(datablock_info.name);
  result.add_group_name(indexer_entry->group_name);
  result.add_idcode(indexer_entry->idcode);

  const AssetMetaData &asset_data = *datablock_info.asset_data;
  result.add_catalog_id(asset_data.catalog_id);
  result.add_catalog_name(asset_data.catalog_simple_name);

  if (asset_data.description != nullptr) {
    result.add_description(asset_data.description);
  }

  if (!BLI_listbase_is_empty(&asset_data.tags)) {
    result.add_tags(&asset_data.tags);
  }

  /* TODO: asset_data.IDProperties */
}

static void init_value_from_file_indexer_entries(blender::io::serialize::ObjectValue &result,
                                                 const FileIndexerEntries &indexer_entries)
{

  blender::io::serialize::ArrayValue *entries = new blender::io::serialize::ArrayValue();
  blender::io::serialize::ArrayValue::Items &items = entries->elements();

  for (LinkNode *ln = indexer_entries.entries; ln; ln = ln->next) {
    const FileIndexerEntry *indexer_entry = static_cast<const FileIndexerEntry *>(ln->link);
    /* TODO: We also get none asset types (Brushes/Workspaces), this seems like an implementation
     * flaw. */
    if (indexer_entry->datablock_info.asset_data == nullptr) {
      continue;
    }
    blender::io::serialize::ObjectValue *entry_value = new blender::io::serialize::ObjectValue();
    AssetEntryWriter entry(*entry_value);
    init_value_from_file_indexer_entry(entry, indexer_entry);
    items.append_as(entry_value);
  }

  /* When no entries to index, we should not store the entries attribute as this would make the
   * size bigger than the MIN_FILE_SIZE_WITH_ENTRIES. */
  if (items.is_empty()) {
    delete entries;
  }
  else {
    blender::io::serialize::ObjectValue::Items &attributes = result.elements();
    attributes.append_as(std::pair(ATTRIBUTE_ENTRIES, entries));
  }
}

static void init_indexer_entry_from_value(FileIndexerEntry &indexer_entry,
                                          const AssetEntryReader &entry)
{
  indexer_entry.idcode = entry.get_idcode();

  const std::string &group_name = entry.get_group_name();
  BLI_strncpy(indexer_entry.group_name, group_name.c_str(), sizeof(indexer_entry.group_name));

  const std::string &name = entry.get_name();
  BLI_strncpy(
      indexer_entry.datablock_info.name, name.c_str(), sizeof(indexer_entry.datablock_info.name));

  AssetMetaData *asset_data = static_cast<AssetMetaData *>(
      MEM_callocN(sizeof(AssetMetaData), __func__));
  indexer_entry.datablock_info.asset_data = asset_data;

  if (entry.has_description()) {
    const std::string &description = entry.get_description();
    char *description_c_str = static_cast<char *>(MEM_mallocN(description.length() + 1, __func__));
    BLI_strncpy(description_c_str, description.c_str(), description.length() + 1);
    asset_data->description = description_c_str;
  }

  const std::string &catalog_name = entry.get_catalog_name();
  BLI_strncpy(asset_data->catalog_simple_name,
              catalog_name.c_str(),
              sizeof(asset_data->catalog_simple_name));

  bUUID catalog_uuid = entry.get_catalog_id();
  asset_data->catalog_id = catalog_uuid;

  entry.get_tags(&asset_data->tags);

  /* TODO: ID properties. */
}

static int init_indexer_entries_from_value(FileIndexerEntries &indexer_entries,
                                           const blender::io::serialize::ObjectValue &value)
{
  int num_entries_read = 0;
  const blender::io::serialize::ObjectValue::Lookup attributes = value.create_lookup();
  const blender::io::serialize::ObjectValue::LookupValue *entries_value = attributes.lookup_ptr(
      ATTRIBUTE_ENTRIES);
  BLI_assert(entries_value != nullptr);

  if (entries_value == nullptr) {
    return num_entries_read;
  }

  const blender::io::serialize::ArrayValue::Items elements =
      (*entries_value)->as_array_value()->elements();
  for (blender::io::serialize::ArrayValue::Item element : elements) {
    const AssetEntryReader asset_entry(*element->as_object_value());

    FileIndexerEntry *entry = static_cast<FileIndexerEntry *>(
        MEM_callocN(sizeof(FileIndexerEntry), __func__));
    init_indexer_entry_from_value(*entry, asset_entry);

    BLI_linklist_prepend(&indexer_entries.entries, entry);
    num_entries_read += 1;
  }

  return num_entries_read;
}

struct AssetLibraryIndex {
  bUserAssetLibrary *library;
  Set<std::string> unused_file_indexes;

 public:
  AssetLibraryIndex(const AssetLibraryReference *library_ref)
  {
    library = BKE_preferences_asset_library_find_from_index(&U, library_ref->custom_library_index);
  }

  const StringRefNull get_name() const
  {
    return StringRefNull(library->name);
  }

  uint64_t hash() const
  {
    DefaultHash<StringRefNull> hasher;
    return hasher(get_library_file_path());
  }

  const StringRefNull get_library_file_path() const
  {
    return StringRefNull(library->path);
  }

  std::string get_index_path() const
  {
    std::stringstream ss;
    ss << BKE_tempdir_base() << "blender-asset-library/";
    ss << std::setfill('0') << std::setw(16) << std::hex << hash() << "_" << get_name() << "/";
    return ss.str();
  }

  std::string index_file_path(const AssetFile &asset_file) const
  {
    /* TODO: Although `BKE_tempdir_base` is documented as persistent temp dir, it ain't! Expected
     * it to return `/var/tmp/` on linux. */
    std::stringstream ss;
    ss << get_index_path();
    ss << std::setfill('0') << std::setw(16) << std::hex << asset_file.hash() << "_"
       << asset_file.get_name() << ".index.json";
    return ss.str();
  }

  void init_unused_index_files()
  {
    std::string index_path = get_index_path();
    if (!BLI_exists(index_path.c_str()) || !BLI_is_dir(index_path.c_str())) {
      return;
    }
    struct direntry *dir_entries = nullptr;
    int num_entries = BLI_filelist_dir_contents(index_path.c_str(), &dir_entries);
    for (int i = 0; i < num_entries; i++) {
      struct direntry *entry = &dir_entries[i];
      if (BLI_str_endswith(entry->relname, ".index.json")) {
        unused_file_indexes.add_as(std::string(entry->path));
      }
    }

    BLI_filelist_free(dir_entries, num_entries);
  }

  void mark_used(std::string &file_name)
  {
    unused_file_indexes.remove(file_name);
  }

  int remove_unused_index_files() const
  {
    int num_files_deleted = 0;
    for (const std::string &unused_index : unused_file_indexes) {
      const char *file_path = unused_index.c_str();
      CLOG_INFO(&LOG, 2, "Remove unused index file [%s].", file_path);
      BLI_delete(file_path, false, false);
      num_files_deleted++;
    }

    return num_files_deleted;
  }
};

struct AssetIndex {
  const int LAST_VERSION = 1;
  const int UNKNOWN_VERSION = -1;

  blender::io::serialize::Value *data;

  AssetIndex(const FileIndexerEntries &indexer_entries)
  {
    blender::io::serialize::ObjectValue *root = new blender::io::serialize::ObjectValue();
    data = root;
    blender::io::serialize::ObjectValue::Items &root_attributes = root->elements();
    root_attributes.append_as(
        std::pair(ATTRIBUTE_VERSION, new blender::io::serialize::IntValue(LAST_VERSION)));

    init_value_from_file_indexer_entries(*root, indexer_entries);
  }

  AssetIndex(blender::io::serialize::Value *value) : data(value)
  {
  }

  virtual ~AssetIndex()
  {
    delete data;
  }

  const int get_version() const
  {
    const blender::io::serialize::ObjectValue *root = data->as_object_value();
    const blender::io::serialize::ObjectValue::Lookup attributes = root->create_lookup();
    const blender::io::serialize::ObjectValue::LookupValue *version_value = attributes.lookup_ptr(
        ATTRIBUTE_VERSION);
    if (version_value == nullptr) {
      return UNKNOWN_VERSION;
    }
    return (*version_value)->as_int_value()->value();
  }

  const bool is_latest_version() const
  {
    return get_version() == LAST_VERSION;
  }

  const int extract_into(FileIndexerEntries &indexer_entries) const
  {
    const blender::io::serialize::ObjectValue *root = data->as_object_value();
    int num_entries_read = init_indexer_entries_from_value(indexer_entries, *root);
    return num_entries_read;
  }
};

class AssetIndexFile : public File {
 public:
  AssetLibraryIndex &library_index;
  /* Asset index files with a size smaller than this attribute would be considered to not contain
   * any entries. */
  const size_t MIN_FILE_SIZE_WITH_ENTRIES = 32;
  std::string file_name;

  AssetIndexFile(AssetLibraryIndex &library_index, AssetFile &asset_file_name)
      : library_index(library_index), file_name(library_index.index_file_path(asset_file_name))
  {
  }

  void mark_used()
  {
    library_index.mark_used(file_name);
  }

  const char *get_file_path() const override
  {
    return file_name.c_str();
  }

  const bool is_older(AssetFile &asset_file) const
  {
    return BLI_file_older(get_file_path(), asset_file.get_file_path());
  }

  /* Check if the index file contains entries without opening the file. */
  const bool constains_entries() const
  {
    const size_t file_size = get_file_size();
    return file_size > MIN_FILE_SIZE_WITH_ENTRIES;
  }

  std::optional<AssetIndex> read_contents() const
  {
    blender::io::serialize::JsonFormatter formatter;
    std::ifstream is;
    is.open(file_name);
    blender::io::serialize::Value *read_data = formatter.deserialize(is);
    is.close();

    return std::make_optional<AssetIndex>(read_data);
  }

  bool ensure_parent_path_exists() const
  {
    /* `BLI_make_existing_file` only ensures parent path, otherwise than expected from the name of
     * the function. */
    return BLI_make_existing_file(get_file_path());
  }

  void write_contents(AssetIndex &content)
  {
    blender::io::serialize::JsonFormatter formatter;
#ifdef DEBUG
    formatter.indentation_len = 2;
#endif
    if (!ensure_parent_path_exists()) {
      CLOG_ERROR(&LOG, "Index not created: couldn't create folder [%s].", get_file_path());
      return;
    }

    std::ofstream os;
    os.open(file_name, std::ios::out | std::ios::trunc);
    formatter.serialize(os, *content.data);
    os.close();
  }
};

static eFileIndexerResult read_index(const char *file_name,
                                     FileIndexerEntries *entries,
                                     int *r_read_entries_len,
                                     void *user_data)
{
  AssetLibraryIndex &library_index = *static_cast<AssetLibraryIndex *>(user_data);
  AssetFile asset_file(file_name);
  AssetIndexFile asset_index_file(library_index, asset_file);

  if (!asset_index_file.exists()) {
    return FILE_INDEXER_NEEDS_UPDATE;
  }

  /* Mark index to be used. Even when the index will be recreated it should still mark the index as
   * used. When not done it would remove the index when the indexing has finished (see
   * `AssetLibraryIndex.remove_unused_index_files`) .*/
  asset_index_file.mark_used();

  if (asset_index_file.is_older(asset_file)) {
    CLOG_INFO(
        &LOG,
        3,
        "Asset index file [%s] needs to be refreshed as it is older than the asset file [%s].",
        asset_index_file.file_name.c_str(),
        file_name);
    return FILE_INDEXER_NEEDS_UPDATE;
  }

  if (!asset_index_file.constains_entries()) {
    CLOG_INFO(&LOG,
              3,
              "Asset file index is to small to contain any entries. [%s]",
              asset_index_file.file_name.c_str());
    *r_read_entries_len = 0;
    return FILE_INDEXER_READ_FROM_INDEX;
  }

  std::optional<AssetIndex> contents = asset_index_file.read_contents();
  if (!contents.has_value()) {
    CLOG_WARN(&LOG,
              "Couldn't read/parse the contents of asset file index [%s].",
              asset_index_file.file_name.c_str());
    return FILE_INDEXER_NEEDS_UPDATE;
  }

  if (!contents->is_latest_version()) {
    CLOG_INFO(&LOG,
              3,
              "Asset file index is ignored due to version mismatch [%s].",
              asset_index_file.file_name.c_str());
    return FILE_INDEXER_NEEDS_UPDATE;
  }

  int read_entries_len = contents->extract_into(*entries);
  CLOG_INFO(&LOG, 1, "Read %d entries from asset index for [%s].", read_entries_len, file_name);
  *r_read_entries_len = read_entries_len;

  return FILE_INDEXER_READ_FROM_INDEX;
}

static void update_index(const char *file_name, FileIndexerEntries *entries, void *user_data)
{
  AssetLibraryIndex &library_index = *static_cast<AssetLibraryIndex *>(user_data);
  AssetFile asset_file(file_name);
  AssetIndexFile asset_index_file(library_index, asset_file);
  CLOG_INFO(&LOG,
            1,
            "Update asset index for [%s] store index in [%s].",
            asset_file.get_file_path(),
            asset_index_file.get_file_path());

  AssetIndex content(*entries);
  asset_index_file.write_contents(content);
}

static void *init_user_data(const AssetLibraryReference *library_reference)
{
  AssetLibraryIndex *library_index = new AssetLibraryIndex(library_reference);
  library_index->init_unused_index_files();
  return library_index;
}

static void free_user_data(void *user_data)
{
  AssetLibraryIndex *index = static_cast<AssetLibraryIndex *>(user_data);
  delete (index);
}

static void filelist_finished(void *user_data)
{
  AssetLibraryIndex &library_index = *static_cast<AssetLibraryIndex *>(user_data);
  int num_indexes_removed = library_index.remove_unused_index_files();
  if (num_indexes_removed > 0) {
    CLOG_INFO(&LOG, 1, "Removed %d unused indexes.", num_indexes_removed);
  }
}

constexpr FileIndexer asset_indexer()
{
  FileIndexer indexer = {nullptr};
  indexer.read_index = read_index;
  indexer.update_index = update_index;
  indexer.init_user_data = init_user_data;
  indexer.free_user_data = free_user_data;
  indexer.filelist_finished = filelist_finished;
  return indexer;
}

}  // namespace blender::ed::asset

extern "C" {

const FileIndexer file_indexer_asset = blender::ed::asset::asset_indexer();
}
