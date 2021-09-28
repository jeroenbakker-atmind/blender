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

#include "asset_indexer.hh"

#include "BLI_fileops.h"
#include "BLI_path_util.h"
#include "BLI_string_ref.hh"

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

struct AssetIndex {
  const int LAST_VERSION = 1;
  int version = LAST_VERSION;

  AssetIndex(const FileIndexerEntries &indexer_entries)
  {
  }

  const bool is_latest_version() const
  {
    /* TODO: check actual version */
    return true;
  }
};

class AssetIndexFile : public File {
 public:
  const size_t MIN_FILE_SIZE_WITH_ENTRIES = 10;
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
    return std::nullopt;
  }

  void write_contents(AssetIndex &content)
  {
  }
};

static eFileIndexerResult read_index(const char *file_name,
                                     FileIndexerEntries *UNUSED(entries),
                                     int *UNUSED(r_read_entries_len))
{
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

  return FILE_INDEXER_READ_FROM_INDEX;
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

const FileIndexer file_indexer_asset = asset_indexer();

}  // namespace blender::ed::asset
