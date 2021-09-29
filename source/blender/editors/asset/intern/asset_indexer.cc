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

#include "BLI_fileops.h"
#include "BLI_linklist.h"
#include "BLI_path_util.h"
#include "BLI_serialize.hh"
#include "BLI_string_ref.hh"
#include "BLI_uuid.h"

#include "DNA_asset_types.h"

#include <fstream>
#include <optional>

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

  std::string index_file_path()
  {
    /* TODO: Should be stored in local machine cache. */
    return file_name + ".index.json";
  }

  const char *get_file_path() const override
  {
    return file_name.c_str();
  }
};

static void build_value_from_file_indexer_entry(blender::io::serialize::ObjectValue &result,
                                                const FileIndexerEntry *indexer_entry)
{
  const BLODataBlockInfo &datablock_info = indexer_entry->datablock_info;
  blender::io::serialize::ObjectValue::Items &attributes = result.elements();
  attributes.append_as(std::pair(std::string("name"),
                                 new blender::io::serialize::StringValue(datablock_info.name)));
  attributes.append_as(
      std::pair(std::string("group_name"),
                new blender::io::serialize::StringValue(indexer_entry->group_name)));
  attributes.append_as(std::pair(std::string("idcode"),
                                 new blender::io::serialize::IntValue(indexer_entry->idcode)));

  const AssetMetaData &asset_data = *datablock_info.asset_data;

  char catalog_id[UUID_STRING_LEN];
  BLI_uuid_format(catalog_id, asset_data.catalog_id);
  attributes.append_as(
      std::pair(std::string("catalog_id"), new blender::io::serialize::StringValue(catalog_id)));

  attributes.append_as(
      std::pair(std::string("catalog_name"),
                new blender::io::serialize::StringValue(asset_data.catalog_simple_name)));

  if (asset_data.description != nullptr) {
    attributes.append_as(
        std::pair(std::string("description"),
                  new blender::io::serialize::StringValue(asset_data.description)));
  }

  if (!BLI_listbase_is_empty(&asset_data.tags)) {
    blender::io::serialize::ArrayValue *tags = new blender::io::serialize::ArrayValue();
    attributes.append_as(std::pair(std::string("tags"), tags));
    blender::io::serialize::ArrayValue::Items &tag_items = tags->elements();

    LISTBASE_FOREACH (AssetTag *, tag, &asset_data.tags) {
      tag_items.append_as(new blender::io::serialize::StringValue(tag->name));
    }
  }

  /* TODO: asset_data.IDProperties */
}

static void build_value_from_file_indexer_entries(blender::io::serialize::ObjectValue &result,
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
    build_value_from_file_indexer_entry(*entry_value, indexer_entry);
    items.append_as(entry_value);
  }

  /* When no entries to index, we should not store the entries attribute as this would make the
   * size bigger than the MIN_FILE_SIZE_WITH_ENTRIES. */
  if (items.is_empty()) {
    delete entries;
  }
  else {
    blender::io::serialize::ObjectValue::Items &attributes = result.elements();
    attributes.append_as(std::pair(std::string("entries"), entries));
  }
}

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
        std::pair(std::string("version"), new blender::io::serialize::IntValue(LAST_VERSION)));

    build_value_from_file_indexer_entries(*root, indexer_entries);
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
    const blender::io::serialize::ObjectValue::LookupValue *Item version_value =
        attributes.lookup_ptr(std::string("version"));
    if (version_value == nullptr) {
      return UNKNOWN_VERSION;
    }
    return (*version_value)->get_int_value().get_value();
  }

  const bool is_latest_version() const
  {
    return get_version() == LAST_VERSION;
  }

  const int extract_into(FileIndexerEntries &indexer_entries) const
  {
    return 0;
  }
};

class AssetIndexFile : public File {
 public:
  const size_t MIN_FILE_SIZE_WITH_ENTRIES = 32;
  std::string file_name;

  AssetIndexFile(AssetFile &asset_file_name) : file_name(asset_file_name.index_file_path())
  {
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
    return get_file_size() < MIN_FILE_SIZE_WITH_ENTRIES;
  }

  std::optional<AssetIndex> read_contents() const
  {
    blender::io::serialize::JsonFormatter formatter;
    std::ifstream is;
    is.open(file_name);
    blender::io::serialize::Value *result = formatter.deserialize(is);
    is.close();

    // TODO: deserialize. Perhaps make AssetIndex->data a pointer.

    return std::nullopt;
  }

  void write_contents(AssetIndex &content)
  {
    blender::io::serialize::JsonFormatter formatter;
#ifdef DEBUG
    formatter.indentation_len = 2;
#endif
    std::ofstream os;
    os.open(file_name, std::ios::out | std::ios::trunc);
    formatter.serialize(os, *content.data);
    os.close();
  }
};

static eFileIndexerResult read_index(const char *file_name,
                                     FileIndexerEntries *entries,
                                     int *r_read_entries_len)
{
#if 1
  return FILE_INDEXER_NEEDS_UPDATE;
#else
  AssetFile asset_file(file_name);
  AssetIndexFile asset_index_file(asset_file);

  if (!asset_index_file.exists()) {
    return FILE_INDEXER_NEEDS_UPDATE;
  }

  if (asset_index_file.is_older(asset_file)) {
    return FILE_INDEXER_NEEDS_UPDATE;
  }

  if (!asset_index_file.constains_entries()) {
    return FILE_INDEXER_READ_FROM_INDEX;
  }

  std::optional<AssetIndex> contents = asset_index_file.read_contents();
  if (!contents.has_value()) {
    return FILE_INDEXER_NEEDS_UPDATE;
  }

  if (!contents->is_latest_version()) {
    return FILE_INDEXER_NEEDS_UPDATE;
  }

  *r_read_entries_len = contents->extract_into(*entries);

  return FILE_INDEXER_READ_FROM_INDEX;
#endif
}

static void update_index(const char *file_name, FileIndexerEntries *entries)
{
  AssetFile asset_file(file_name);
  AssetIndexFile asset_index_file(asset_file);

  AssetIndex content(*entries);
  asset_index_file.write_contents(content);
}

constexpr FileIndexer asset_indexer()
{
  FileIndexer indexer = {nullptr};
  indexer.read_index = read_index;
  indexer.update_index = update_index;
  return indexer;
}

}  // namespace blender::ed::asset

extern "C" {

const FileIndexer file_indexer_asset = blender::ed::asset::asset_indexer();
}
