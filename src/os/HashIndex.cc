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

#include "include/types.h"
#include "include/buffer.h"
#include "osd/osd_types.h"

#include "HashIndex.h"

const string HashIndex::SUBDIR_ATTR = "contents";
const string HashIndex::IN_PROGRESS_OP_TAG = "in_progress_op";

int HashIndex::cleanup() {
  bufferlist bl;
  int r = get_attr_path(vector<string>(), IN_PROGRESS_OP_TAG, bl);
  if (r < 0) {
    // No in progress operations!
    return 0;
  }
  bufferlist::iterator i = bl.begin();
  InProgressOp in_progress(i);
  subdir_info_s info;
  r = get_info(in_progress.path, &info);
  if (r < 0)
    return r;
  if (in_progress.is_split())
    return complete_split(in_progress.path, info);
  else if (in_progress.is_merge())
    return complete_merge(in_progress.path, info);
  else
    return -EINVAL;
}

int HashIndex::_init() {
  subdir_info_s info;
  vector<string> path;
  return set_info(path, info);
}

/* LFNIndex virtual method implementations */
int HashIndex::_created(const vector<string> &path,
			const hobject_t &hoid,
			const string &mangled_name) {
  subdir_info_s info;
  int r;
  r = get_info(path, &info);
  if (r < 0)
    return r;
  info.objs++;
  r = set_info(path, info);
  if (r < 0)
    return r;

  if (must_split(info)) {
    int r = initiate_split(path, info);
    if (r < 0)
      return r;
    return complete_split(path, info);
  } else {
    return 0;
  }
}

int HashIndex::_remove(const vector<string> &path,
		       const hobject_t &hoid,
		       const string &mangled_name) {
  int r;
  r = remove_object(path, hoid);
  if (r < 0)
    return r;
  subdir_info_s info;
  r = get_info(path, &info);
  if (r < 0)
    return r;
  info.objs--;
  r = set_info(path, info);
  if (r < 0)
    return r;
  if (must_merge(info)) {
    r = initiate_merge(path, info);
    if (r < 0)
      return r;
    return complete_merge(path, info);
  } else {
    return 0;
  }
}

int HashIndex::_lookup(const hobject_t &hoid,
		       vector<string> *path,
		       string *mangled_name,
		       int *exists_out) {
  vector<string> path_comp;
  get_path_components(hoid, &path_comp);
  vector<string>::iterator next = path_comp.begin();
  int r, exists;
  while (1) {
    r = path_exists(*path, &exists);
    if (r < 0)
      return r;
    if (!exists) {
      if (path->empty())
	return -ENOENT;
      path->pop_back();
      break;
    }
    if (next == path_comp.end())
      break;
    path->push_back(*(next++));
  }
  return get_mangled_name(*path, hoid, mangled_name, exists_out);
}

static void split_handle(collection_list_handle_t handle, 
			 uint32_t *hash, uint32_t *index) {
  *hash = handle.hash;
  *index = handle.index;
}

static collection_list_handle_t form_handle(uint32_t hash, uint32_t index) {
  collection_list_handle_t handle;
  handle.hash = hash;
  handle.index = index;
  return handle;
}

int HashIndex::_collection_list_partial(snapid_t seq, int max_count,
					vector<hobject_t> *ls, 
					collection_list_handle_t *last) {
  vector<string> path;
  uint32_t index, hash;
  string lower_bound;
  if (last) {
    split_handle(*last, &hash, &index);
    lower_bound = get_hash_str(hash);
  }
  int r = list(path, 
	       max_count ? &max_count : NULL, 
	       &seq,
	       last ? &lower_bound : NULL,
	       last ? &index : NULL, 
	       ls);
  if (r < 0)
    return r;
  if (last && ls->size())
    *last = form_handle(ls->rbegin()->hash, index);
  return 0;
}

int HashIndex::_collection_list(vector<hobject_t> *ls) {
  vector<string> path;
  return list(path, NULL, NULL, NULL, NULL, ls);
}

int HashIndex::start_split(const vector<string> &path) {
  bufferlist bl;
  InProgressOp op_tag(InProgressOp::SPLIT, path);
  op_tag.encode(bl);
  return add_attr_path(vector<string>(), IN_PROGRESS_OP_TAG, bl); 
}

int HashIndex::start_merge(const vector<string> &path) {
  bufferlist bl;
  InProgressOp op_tag(InProgressOp::MERGE, path);
  op_tag.encode(bl);
  return add_attr_path(vector<string>(), IN_PROGRESS_OP_TAG, bl); 
}

int HashIndex::end_split_or_merge(const vector<string> &path) {
  return remove_attr_path(vector<string>(), IN_PROGRESS_OP_TAG);
}

int HashIndex::get_info(const vector<string> &path, subdir_info_s *info) {
  bufferlist buf;
  int r = get_attr_path(path, SUBDIR_ATTR, buf);
  if (r < 0)
    return r;
  bufferlist::iterator bufiter = buf.begin();
  info->decode(bufiter);
  assert(path.size() == (unsigned)info->hash_level);
  return 0;
}

int HashIndex::set_info(const vector<string> &path, const subdir_info_s &info) {
  bufferlist buf;
  assert(path.size() == (unsigned)info.hash_level);
  info.encode(buf);
  return add_attr_path(path, SUBDIR_ATTR, buf);
}

bool HashIndex::must_merge(const subdir_info_s &info) {
  return (info.hash_level > 0 &&
	  info.objs < (unsigned)merge_threshold &&
	  info.subdirs == 0);
}

bool HashIndex::must_split(const subdir_info_s &info) {
  return (info.hash_level < (unsigned)MAX_HASH_LEVEL &&
	  info.objs > ((unsigned)merge_threshold * 16 * split_multiplier));
			    
}

int HashIndex::initiate_merge(const vector<string> &path, subdir_info_s info) {
  return start_merge(path);
}

int HashIndex::complete_merge(const vector<string> &path, subdir_info_s info) {
  vector<string> dst = path;
  dst.pop_back();
  subdir_info_s dstinfo;
  int r, exists;
  r = path_exists(path, &exists);
  if (r < 0)
    return r;
  r = get_info(dst, &dstinfo);
  if (r < 0)
    return r;
  if (exists) {
    r = move_objects(path, dst);
    if (r < 0)
      return r;
    
    map<string,hobject_t> objects_dst;
    r = list_objects(dst, 0, 0, &objects_dst);
    if (r < 0)
      return r;
    set<string> subdirs;
    r = list_subdirs(dst, &subdirs);
    if (r < 0)
      return r;
    dstinfo.objs = objects_dst.size();
    dstinfo.subdirs = subdirs.size() - 1;
    r = set_info(dst, dstinfo);
    if (r < 0)
      return r;
    r = remove_path(path);
    if (r < 0)
      return r;
  }
  if (must_merge(dstinfo)) {
    r = initiate_merge(dst, dstinfo);
    if (r < 0)
      return r;
    r = fsync_dir(dst);
    if (r < 0)
      return r;
    return complete_merge(dst, dstinfo);
  }
  r = fsync_dir(dst);
  if (r < 0)
    return r;
  return end_split_or_merge(path);
}

int HashIndex::initiate_split(const vector<string> &path, subdir_info_s info) {
  return start_split(path);
}

int HashIndex::complete_split(const vector<string> &path, subdir_info_s info) {
  int level = info.hash_level;
  map<string, hobject_t> objects;
  vector<string> dst = path;
  int r;
  dst.push_back("");
  r = list_objects(path, 0, 0, &objects);
  if (r < 0)
    return r;
  set<string> subdirs;
  r = list_subdirs(path, &subdirs);
  if (r < 0)
    return r;
  map<string, map<string, hobject_t> > mapped;
  map<string, hobject_t> moved;
  int num_moved = 0;
  for (map<string, hobject_t>::iterator i = objects.begin();
       i != objects.end();
       ++i) {
    vector<string> new_path;
    get_path_components(i->second, &new_path);
    mapped[new_path[level]][i->first] = i->second;
  }
  for (map<string, map<string, hobject_t> >::iterator i = mapped.begin();
       i != mapped.end();
       ) {
    dst[level] = i->first;
    /* If the info already exists, it must be correct,
     * we may be picking up a partially finished split */
    subdir_info_s temp;
    // subdir has already been fully copied
    if (subdirs.count(i->first) && !get_info(dst, &temp)) {
      for (map<string, hobject_t>::iterator j = i->second.begin();
	   j != i->second.end();
	   ++j) {
	moved[j->first] = j->second;
	num_moved++;
	objects.erase(j->first);
      }
      ++i;
      continue;
    }

    subdir_info_s info_new;
    info_new.objs = i->second.size();
    info_new.subdirs = 0;
    info_new.hash_level = level + 1;
    if (must_merge(info_new) && !subdirs.count(i->first)) {
      mapped.erase(i++);
      continue;
    }

    // Subdir doesn't yet exist
    if (!subdirs.count(i->first)) {
      info.subdirs += 1;
      r = create_path(dst);
      if (r < 0)
	return r;
    } // else subdir has been created but only partially copied

    for (map<string, hobject_t>::iterator j = i->second.begin();
	 j != i->second.end();
	 ++j) {
      moved[j->first] = j->second;
      num_moved++;
      objects.erase(j->first);
      r = link_object(path, dst, j->second, j->first);
      // May be a partially finished split
      if (r < 0 && r != -EEXIST) {
	return r;
      }
    }

    r = fsync_dir(dst);
    if (r < 0)
      return r;

    // Presence of info must imply that all objects have been copied
    r = set_info(dst, info_new);
    if (r < 0)
      return r;

    r = fsync_dir(dst);
    if (r < 0)
      return r;

    ++i;
  }
  r = remove_objects(path, moved, &objects);
  if (r < 0)
    return r;
  info.objs = objects.size();
  r = set_info(path, info);
  if (r < 0)
    return r;
  r = fsync_dir(path);
  if (r < 0)
    return r;
  return end_split_or_merge(path);
}

void HashIndex::get_path_components(const hobject_t &hoid,
				    vector<string> *path) {
  char buf[MAX_HASH_LEVEL + 1];
  snprintf(buf, sizeof(buf), "%.*X", MAX_HASH_LEVEL, hoid.hash);

  // Path components are the hex characters of hoid.hash in, least
  // significant first
  for (int i = 0; i < MAX_HASH_LEVEL; ++i) {
    path->push_back(string(&buf[MAX_HASH_LEVEL - 1 - i], 1));
  }
}

string HashIndex::get_hash_str(uint32_t hash) {
  char buf[MAX_HASH_LEVEL + 1];
  snprintf(buf, sizeof(buf), "%.*X", MAX_HASH_LEVEL, hash);
  string retval;
  for (int i = 0; i < MAX_HASH_LEVEL; ++i) {
    retval.push_back(buf[MAX_HASH_LEVEL - 1 - i]);
  }
  return retval;
}

string HashIndex::get_path_str(const hobject_t &hoid) {
  return get_hash_str(hoid.hash);
}

int HashIndex::list(const vector<string> &path,
		    const int *max_count,
		    const snapid_t *seq,
		    const string *lower_bound,
		    uint32_t *index,
		    vector<hobject_t> *out) {
  if (lower_bound)
    assert(index);
  vector<string> next_path = path;
  next_path.push_back("");
  set<string> hash_prefixes;
  multimap<string, hobject_t> objects;
  map<string, hobject_t> rev_objects;
  int r;
  int max = max_count ? *max_count : 0;
  string cur_prefix;
  for (vector<string>::const_iterator i = path.begin();
       i != path.end();
       ++i) {
    cur_prefix.append(*i);
  }

  r = list_objects(path, 0, 0, &rev_objects);
  if (r < 0)
    return r;
  for (map<string, hobject_t>::iterator i = rev_objects.begin();
       i != rev_objects.end();
       ++i) {
    string hash_prefix = get_path_str(i->second);
    if (lower_bound && hash_prefix < *lower_bound)
      continue;
    if (seq && i->second.snap < *seq)
      continue;
    hash_prefixes.insert(hash_prefix);
    objects.insert(pair<string, hobject_t>(hash_prefix, i->second));
  }
  set<string> subdirs;
  r = list_subdirs(path, &subdirs);
  if (r < 0)
    return r;
  for (set<string>::iterator i = subdirs.begin();
       i != subdirs.end();
       ++i) {
    string candidate = cur_prefix + *i;
    if (lower_bound && candidate < lower_bound->substr(0, candidate.size()))
      continue;
    hash_prefixes.insert(cur_prefix + *i);
  }

  uint32_t counter = 0;
  for (set<string>::iterator i = hash_prefixes.begin();
       i != hash_prefixes.end() && (!max_count || max > 0);
       ++i) {
    multimap<string, hobject_t>::iterator j = objects.find(*i);
    if (j != objects.end()) {
      counter = 0;
      for (; (!max_count || max > 0) && j != objects.end() && j->first == *i; ++j, ++counter) {
	if (lower_bound && *lower_bound == *i && counter < *index)
	  continue;
	out->push_back(j->second);
	if (max_count)
	  max--;
      }
      if (index)
	*index = counter;
      continue;
    } 

    // subdir
    *(next_path.rbegin()) = *(i->rbegin());
    int old_size = out->size();
    assert(next_path.size() > path.size());
    r = list(next_path, max_count ? &max : NULL, seq, lower_bound, index, out);
    if (r < 0)
      return r;
    if (max_count)
      max -= out->size() - old_size;
  }
  return 0;
}
