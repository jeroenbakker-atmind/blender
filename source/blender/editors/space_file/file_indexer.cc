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
 * \ingroup edfile
 *
 * This file implements the default file browser indexer.
 *
 * The default indexer doesn't index anything.
 */
#include "file_indexer.h"

#include "BLI_linklist.h"
#include "BLI_listbase.h"
#include "BLI_utildefines.h"

namespace blender::editor::file::indexer {

static eFileIndexerResult read_index(const char *UNUSED(file_name),
                                     LinkNode /* FileIndexerEntry */ **UNUSED(entries),
                                     int *UNUSED(r_read_entries_len))
{
  return FILE_INDEXER_NEEDS_UPDATE;
}

static void update_index(const char *UNUSED(file_name),
                         LinkNode /* FileIndexerEntry */ *UNUSED(entries))
{
}

constexpr FileIndexer default_indexer()
{
  FileIndexer indexer = {nullptr};
  indexer.read_index = read_index;
  indexer.update_index = update_index;
  return indexer;
}

}  // namespace blender::editor::file::indexer

extern "C" {
FileIndexer file_indexer_default = blender::editor::file::indexer::default_indexer();
}
