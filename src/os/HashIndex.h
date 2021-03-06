// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_HASHINDEX_H
#define CEPH_HASHINDEX_H

#include "include/buffer.h"
#include "include/encoding.h"
#include "LFNIndex.h"


/**
 * Implements collection prehashing.
 *     (root) - 0 - 0
 *                - 1
 *                - E
 *            - 1
 *            - 2 - D - 0
 *            .
 *            .
 *            .
 *            - F - 0
 * A file is located at the longest existing directory from the root 
 * given by the hex characters in the hash beginning with the least
 * significant.
 * 
 * ex: hobject_t("object", CEPH_NO_SNAP, 0xA4CEE0D2)
 * would be located in (root)/2/D/0/
 * 
 * Subdirectories are created when the number of objects in a directory
 * exceed 32*merge_threshhold.  The number of objects in a directory 
 * is encoded as subdir_info_s in an xattr on the directory.
 */
class HashIndex : public LFNIndex {
private:
  /// Attribute name for storing subdir info @see subdir_info_s
  static const string SUBDIR_ATTR;
  /// Attribute name for storing in progress op tag
  static const string IN_PROGRESS_OP_TAG;
  /// Size (bits) in object hash
  static const int PATH_HASH_LEN = 32;
  /// Max length of hashed path
  static const int MAX_HASH_LEVEL = (PATH_HASH_LEN/4);

  /**
   * Merges occur when the number of object drops below
   * merge_threshold and splits occur when the number of objects
   * exceeds 16 * merge_threshold * split_multiplier.
   */
  int merge_threshold;
  int split_multiplier;

  /// Encodes current subdir state for determining when to split/merge.
  struct subdir_info_s {
    uint64_t objs;       ///< Objects in subdir.
    uint32_t subdirs;    ///< Subdirs in subdir.
    uint32_t hash_level; ///< Hashlevel of subdir.

    subdir_info_s() : objs(0), subdirs(0), hash_level(0) {}
    
    void encode(bufferlist &bl) const
    {
      __u8 v = 1;
      ::encode(v, bl);
      ::encode(objs, bl);
      ::encode(subdirs, bl);
      ::encode(hash_level, bl);
    }
    
    void decode(bufferlist::iterator &bl)
    {
      __u8 v;
      ::decode(v, bl);
      assert(v == 1);
      ::decode(objs, bl);
      ::decode(subdirs, bl);
      ::decode(hash_level, bl);
    }
  };

  /// Encodes in progress split or merge
  struct InProgressOp {
    static const int SPLIT = 0;
    static const int MERGE = 1;
    int op;
    vector<string> path;

    InProgressOp(int op, const vector<string> &path) 
      : op(op), path(path) {}

    InProgressOp(bufferlist::iterator &bl) {
      decode(bl);
    }

    bool is_split() const { return op == SPLIT; }
    bool is_merge() const { return op == MERGE; }

    void encode(bufferlist &bl) const {
      __u8 v = 1;
      ::encode(v, bl);
      ::encode(op, bl);
      ::encode(path, bl);
    }

    void decode(bufferlist::iterator &bl) {
      __u8 v;
      ::decode(v, bl);
      assert(v == 1);
      ::decode(op, bl);
      ::decode(path, bl);
    }
  };
    
    
public:
  /// Constructor.
  HashIndex(
    const char *base_path, ///< [in] Path to the index root.
    int merge_at,          ///< [in] Merge threshhold.
    int split_multiple,	   ///< [in] Split threshhold.
    uint32_t index_version)///< [in] Index version
    : LFNIndex(base_path, index_version), merge_threshold(merge_at),
      split_multiplier(split_multiple) {}

  /// @see CollectionIndex
  uint32_t collection_version() { return index_version; }

  /// @see CollectionIndex
  int cleanup();
	
protected:
  int _init();

  int _created(
    const vector<string> &path,
    const hobject_t &hoid,
    const string &mangled_name
    );
  int _remove(
    const vector<string> &path,
    const hobject_t &hoid,
    const string &mangled_name
    );
  int _lookup(
    const hobject_t &hoid,
    vector<string> *path,
    string *mangled_name,
    int *exists
    );
  int _collection_list_partial(
    snapid_t seq,
    int max_count,
    vector<hobject_t> *ls, 
    collection_list_handle_t *last
    );
  int _collection_list(
    vector<hobject_t> *ls
    );
private:
  /// Tag root directory at beginning of split
  int start_split(
    const vector<string> &path ///< [in] path to split
    ); ///< @return Error Code, 0 on success
  /// Tag root directory at beginning of split
  int start_merge(
    const vector<string> &path ///< [in] path to merge
    ); ///< @return Error Code, 0 on success
  /// Remove tag at end of split or merge
  int end_split_or_merge(
    const vector<string> &path ///< [in] path to split or merged
    ); ///< @return Error Code, 0 on success
  /// Gets info from the xattr on the subdir represented by path
  int get_info(
    const vector<string> &path, ///< [in] Path from which to read attribute.
    subdir_info_s *info		///< [out] Attribute value
    ); /// @return Error Code, 0 on success

  /// Sets info to the xattr on the subdir represented by path
  int set_info(
    const vector<string> &path, ///< [in] Path on which to set attribute.
    const subdir_info_s &info  	///< [in] Value to set
    ); /// @return Error Code, 0 on success

  /// Encapsulates logic for when to split.
  bool must_merge(
    const subdir_info_s &info ///< [in] Info to check
    ); /// @return True if info must be merged, False otherwise

  /// Encapsulates logic for when to merge.
  bool must_split(
    const subdir_info_s &info ///< [in] Info to check
    ); /// @return True if info must be split, False otherwise

  /// Initiates merge
  int initiate_merge(
    const vector<string> &path, ///< [in] Subdir to merge
    subdir_info_s info		///< [in] Info attached to path
    ); /// @return Error Code, 0 on success

  /// Completes merge
  int complete_merge(
    const vector<string> &path, ///< [in] Subdir to merge
    subdir_info_s info		///< [in] Info attached to path
    ); /// @return Error Code, 0 on success

  /// Initiate Split
  int initiate_split(
    const vector<string> &path, ///< [in] Subdir to split
    subdir_info_s info		///< [in] Info attached to path
    ); /// @return Error Code, 0 on success

  /// Completes Split
  int complete_split(
    const vector<string> &path, ///< [in] Subdir to split
    subdir_info_s info	       ///< [in] Info attached to path
    ); /// @return Error Code, 0 on success

  /// Determine path components from hoid hash
  void get_path_components(
    const hobject_t &hoid, ///< [in] Object for which to get path components
    vector<string> *path   ///< [out] Path components for hoid.
    );

  /** 
   * Get string representation of hobject_t/hash
   *
   * e.g: 0x01234567 -> "76543210"
   */
  string get_path_str(
    const hobject_t &hoid ///< [in] Object to get hash string for
    ); ///< @return Hash string for hoid.

  /// Get string from hash, @see get_path_str
  string get_hash_str(
    uint32_t hash ///< [in] Hash to convert to a string.
    ); ///< @return String representation of hash

  /** 
   * Recursively lists all objects in path.
   *
   * Lists all objects in path or a subdirectory of path which
   * sort greater than *lower_bound and have snapid_t > *seq in order of 
   * hash up to a max count of *max_count
   *
   * In case of multiple objects with the same hash, *index indicates the
   * last listed.  (If non-null, the index of the last object listed will 
   * be assigned to *index).
   *
   * max_count, seq, lower_bound optional, lower_bound iff index
   */
  int list(
    const vector<string> &path, ///< [in] Path to list.
    const int *max_count,	///< [in] Max number to list (NULL if not needed)
    const snapid_t *seq,	///< [in] Snap to list (NULL if not needed)
    const string *lower_bound,	///< [in] Last hash listed (NULL if not needed)
    uint32_t *index,		///< [in,out] last index (NULL iff !lower_bound)
    vector<hobject_t> *out	///< [out] Listed objects
    ); ///< @return Error Code, 0 on success
};

#endif
