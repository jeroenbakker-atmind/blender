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
 */

#pragma once

#include "BLO_readfile.h"

#ifdef __cplusplus
extern "C" {
#endif

struct AssetLibraryReference;
struct LinkNode;

typedef enum eFileIndexerResult {
  FILE_INDEXER_READ_FROM_INDEX,
  FILE_INDEXER_NEEDS_UPDATE,
} eFileIndexerResult;

typedef struct FileIndexerEntry {
  struct BLODataBlockInfo datablock_info;
  short idcode;
} FileIndexerEntry;

typedef struct FileIndexerEntries {
  struct LinkNode /* FileIndexerEntry */ *entries;
} FileIndexerEntries;

typedef void *(*FileIndexerInitUserDataFunc)(const AssetLibraryReference *library_reference);
typedef void (*FileIndexerFreeUserDataFunc)(void *);
typedef void (*FileIndexerFinishedFunc)(void *);
typedef eFileIndexerResult (*FileIndexerReadIndexFunc)(const char *file_name,
                                                       FileIndexerEntries *entries,
                                                       int *r_read_entries_len,
                                                       void *user_data);
typedef void (*FileIndexerUpdateIndexFunc)(const char *file_name,
                                           FileIndexerEntries *entries,
                                           void *user_data);

typedef struct FileIndexer {
  /* Optional callback */
  FileIndexerInitUserDataFunc init_user_data;
  /* Optional callback */
  FileIndexerFreeUserDataFunc free_user_data;
  /* Optional callback. Called when listing files completed. */
  FileIndexerFinishedFunc filelist_finished;

  FileIndexerReadIndexFunc read_index;
  FileIndexerUpdateIndexFunc update_index;
} FileIndexer;

/* file_indexer.cc */
void ED_file_indexer_entries_clear(FileIndexerEntries *indexer_entries);
void ED_file_indexer_entries_extend_from_datablock_infos(
    FileIndexerEntries *indexer_entries,
    const LinkNode * /* BLODataBlockInfo */ datablock_infos,
    const int idcode);

#ifdef __cplusplus
}
#endif
