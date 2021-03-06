//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/version_builder.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <algorithm>
#include <set>
#include <vector>

#include "db/dbformat.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "table/table_reader.h"

namespace rocksdb {

bool NewestFirstBySeqNo(FileMetaData* a, FileMetaData* b) {
  if (a->smallest_seqno != b->smallest_seqno) {
    return a->smallest_seqno > b->smallest_seqno;
  }
  if (a->largest_seqno != b->largest_seqno) {
    return a->largest_seqno > b->largest_seqno;
  }
  // Break ties by file number
  return a->fd.GetNumber() > b->fd.GetNumber();
}

namespace {
bool BySmallestKey(FileMetaData* a, FileMetaData* b,
                   const InternalKeyComparator* cmp) {
  int r = cmp->Compare(a->smallest, b->smallest);
  if (r != 0) {
    return (r < 0);
  }
  // Break ties by file number
  return (a->fd.GetNumber() < b->fd.GetNumber());
}
}  // namespace

class VersionBuilder::Rep {
 private:
  // Helper to sort files_ in v
  // kLevel0 -- NewestFirstBySeqNo
  // kLevelNon0 -- BySmallestKey
  struct FileComparator {
    enum SortMethod {
      kLevel0 = 0,
      kLevelNon0 = 1,
    } sort_method;
    const InternalKeyComparator* internal_comparator;

    bool operator()(FileMetaData* f1, FileMetaData* f2) const {
      switch (sort_method) {
        case kLevel0:
          return NewestFirstBySeqNo(f1, f2);
        case kLevelNon0:
          return BySmallestKey(f1, f2, internal_comparator);
      }
      assert(false);
      return false;
    }
  };

  typedef std::set<FileMetaData*, FileComparator> FileSet;
  struct LevelState {
    std::set<uint64_t> deleted_files;
    FileSet* added_files;
  };

  const EnvOptions& env_options_;
  TableCache* table_cache_;
  VersionStorageInfo* base_vstorage_;
  LevelState* levels_;
  FileComparator level_zero_cmp_;
  FileComparator level_nonzero_cmp_;

 public:
  Rep(const EnvOptions& env_options, TableCache* table_cache,
      VersionStorageInfo* base_vstorage)
      : env_options_(env_options),
        table_cache_(table_cache),
        base_vstorage_(base_vstorage) {
    levels_ = new LevelState[base_vstorage_->NumberLevels()];
    level_zero_cmp_.sort_method = FileComparator::kLevel0;
    level_nonzero_cmp_.sort_method = FileComparator::kLevelNon0;
    level_nonzero_cmp_.internal_comparator =
        base_vstorage_->InternalComparator();

    levels_[0].added_files = new FileSet(level_zero_cmp_);
    for (int level = 1; level < base_vstorage_->NumberLevels(); level++) {
        levels_[level].added_files = new FileSet(level_nonzero_cmp_);
    }
  }

  ~Rep() {
    for (int level = 0; level < base_vstorage_->NumberLevels(); level++) {
      const FileSet* added = levels_[level].added_files;
      std::vector<FileMetaData*> to_unref;
      to_unref.reserve(added->size());
      for (FileSet::const_iterator it = added->begin();
          it != added->end(); ++it) {
        to_unref.push_back(*it);
      }
      delete added;
      for (uint32_t i = 0; i < to_unref.size(); i++) {
        FileMetaData* f = to_unref[i];
        f->refs--;
        if (f->refs <= 0) {
          if (f->table_reader_handle) {
            assert(table_cache_ != nullptr);
            table_cache_->ReleaseHandle(f->table_reader_handle);
            f->table_reader_handle = nullptr;
          }
          delete f;
        }
      }
    }

    delete[] levels_;
  }

  void CheckConsistency(VersionStorageInfo* vstorage) {
#ifndef NDEBUG
    // make sure the files are sorted correctly
    for (int level = 0; level < vstorage->NumberLevels(); level++) {
      auto& level_files = vstorage->LevelFiles(level);
      for (size_t i = 1; i < level_files.size(); i++) {
        auto f1 = level_files[i - 1];
        auto f2 = level_files[i];
        if (level == 0) {
          assert(level_zero_cmp_(f1, f2));
          assert(f1->largest_seqno > f2->largest_seqno);
        } else {
          assert(level_nonzero_cmp_(f1, f2));

          // Make sure there is no overlap in levels > 0
          if (vstorage->InternalComparator()->Compare(f1->largest,
                                                      f2->smallest) >= 0) {
            fprintf(stderr, "overlapping ranges in same level %s vs. %s\n",
                    (f1->largest).DebugString().c_str(),
                    (f2->smallest).DebugString().c_str());
            abort();
          }
        }
      }
    }
#endif
  }

  void CheckConsistencyForDeletes(VersionEdit* edit, uint64_t number,
                                  int level) {
#ifndef NDEBUG
      // a file to be deleted better exist in the previous version
      bool found = false;
      for (int l = 0; !found && l < base_vstorage_->NumberLevels(); l++) {
        const std::vector<FileMetaData*>& base_files =
            base_vstorage_->LevelFiles(l);
        for (unsigned int i = 0; i < base_files.size(); i++) {
          FileMetaData* f = base_files[i];
          if (f->fd.GetNumber() == number) {
            found =  true;
            break;
          }
        }
      }
      // if the file did not exist in the previous version, then it
      // is possibly moved from lower level to higher level in current
      // version
      for (int l = level + 1; !found && l < base_vstorage_->NumberLevels();
           l++) {
        const FileSet* added = levels_[l].added_files;
        for (FileSet::const_iterator added_iter = added->begin();
             added_iter != added->end(); ++added_iter) {
          FileMetaData* f = *added_iter;
          if (f->fd.GetNumber() == number) {
            found = true;
            break;
          }
        }
      }

      // maybe this file was added in a previous edit that was Applied
      if (!found) {
        const FileSet* added = levels_[level].added_files;
        for (FileSet::const_iterator added_iter = added->begin();
             added_iter != added->end(); ++added_iter) {
          FileMetaData* f = *added_iter;
          if (f->fd.GetNumber() == number) {
            found = true;
            break;
          }
        }
      }
      if (!found) {
        fprintf(stderr, "not found %" PRIu64 "\n", number);
      }
      assert(found);
#endif
  }

  // Apply all of the edits in *edit to the current state.
  void Apply(VersionEdit* edit) {
    CheckConsistency(base_vstorage_);

    // Delete files
    const VersionEdit::DeletedFileSet& del = edit->GetDeletedFiles();
    for (const auto& del_file : del) {
      const auto level = del_file.first;
      const auto number = del_file.second;
      levels_[level].deleted_files.insert(number);
      CheckConsistencyForDeletes(edit, number, level);
    }

    // Add new files
    for (const auto& new_file : edit->GetNewFiles()) {
      const int level = new_file.first;
      FileMetaData* f = new FileMetaData(new_file.second);
      f->refs = 1;

      levels_[level].deleted_files.erase(f->fd.GetNumber());
      levels_[level].added_files->insert(f);
    }
  }

  // Save the current state in *v.
  void SaveTo(VersionStorageInfo* vstorage) {
    CheckConsistency(base_vstorage_);
    CheckConsistency(vstorage);

    for (int level = 0; level < base_vstorage_->NumberLevels(); level++) {
      const auto& cmp = (level == 0) ? level_zero_cmp_ : level_nonzero_cmp_;
      // Merge the set of added files with the set of pre-existing files.
      // Drop any deleted files.  Store the result in *v.
      const auto& base_files = base_vstorage_->LevelFiles(level);
      auto base_iter = base_files.begin();
      auto base_end = base_files.end();
      const auto& added_files = *levels_[level].added_files;
      vstorage->Reserve(level, base_files.size() + added_files.size());

      for (const auto& added : added_files) {
        // Add all smaller files listed in base_
        for (auto bpos = std::upper_bound(base_iter, base_end, added, cmp);
             base_iter != bpos;
             ++base_iter) {
          MaybeAddFile(vstorage, level, *base_iter);
        }

        MaybeAddFile(vstorage, level, added);
      }

      // Add remaining base files
      for (; base_iter != base_end; ++base_iter) {
        MaybeAddFile(vstorage, level, *base_iter);
      }
    }

    CheckConsistency(vstorage);
  }

  void LoadTableHandlers() {
    assert(table_cache_ != nullptr);
    for (int level = 0; level < base_vstorage_->NumberLevels(); level++) {
      for (auto& file_meta : *(levels_[level].added_files)) {
        assert(!file_meta->table_reader_handle);
        table_cache_->FindTable(
            env_options_, *(base_vstorage_->InternalComparator()),
            file_meta->fd, &file_meta->table_reader_handle, false);
        if (file_meta->table_reader_handle != nullptr) {
          // Load table_reader
          file_meta->fd.table_reader = table_cache_->GetTableReaderFromHandle(
              file_meta->table_reader_handle);
      }
    }
  }
  }

  void MaybeAddFile(VersionStorageInfo* vstorage, int level, FileMetaData* f) {
    if (levels_[level].deleted_files.count(f->fd.GetNumber()) > 0) {
      // File is deleted: do nothing
    } else {
      vstorage->MaybeAddFile(level, f);
    }
  }
};

VersionBuilder::VersionBuilder(const EnvOptions& env_options,
                               TableCache* table_cache,
                               VersionStorageInfo* base_vstorage)
    : rep_(new Rep(env_options, table_cache, base_vstorage)) {}
VersionBuilder::~VersionBuilder() { delete rep_; }
void VersionBuilder::CheckConsistency(VersionStorageInfo* vstorage) {
  rep_->CheckConsistency(vstorage);
}
void VersionBuilder::CheckConsistencyForDeletes(VersionEdit* edit,
                                                uint64_t number, int level) {
  rep_->CheckConsistencyForDeletes(edit, number, level);
}
void VersionBuilder::Apply(VersionEdit* edit) { rep_->Apply(edit); }
void VersionBuilder::SaveTo(VersionStorageInfo* vstorage) {
  rep_->SaveTo(vstorage);
}
void VersionBuilder::LoadTableHandlers() { rep_->LoadTableHandlers(); }
void VersionBuilder::MaybeAddFile(VersionStorageInfo* vstorage, int level,
                                  FileMetaData* f) {
  rep_->MaybeAddFile(vstorage, level, f);
}

}  // namespace rocksdb
