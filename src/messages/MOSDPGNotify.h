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

#ifndef CEPH_MOSDPGPEERNOTIFY_H
#define CEPH_MOSDPGPEERNOTIFY_H

#include "msg/Message.h"

#include "osd/PG.h"

/*
 * PGNotify - notify primary of my PGs and versions.
 */

class MOSDPGNotify : public Message {
  epoch_t epoch;
  /// query_epoch is the epoch of the query being responded to, or
  /// the current epoch if this is not being sent in response to a
  /// query. This allows the recipient to disregard responses to old
  /// queries.
  epoch_t query_epoch;
  vector<PG::Info> pg_list;   // pgid -> version

 public:
  version_t get_epoch() { return epoch; }
  vector<PG::Info>& get_pg_list() { return pg_list; }
  epoch_t get_query_epoch() { return query_epoch; }

  MOSDPGNotify() {}
  MOSDPGNotify(epoch_t e, vector<PG::Info>& l, epoch_t query_epoch) :
    Message(MSG_OSD_PG_NOTIFY), epoch(e),
    query_epoch(query_epoch) {
    pg_list.swap(l);
  }
private:
  ~MOSDPGNotify() {}

public:  
  const char *get_type_name() { return "PGnot"; }

  void encode_payload(CephContext *cct) {
    header.version = 2;
    ::encode(epoch, payload);
    ::encode(pg_list, payload);
    ::encode(query_epoch, payload);
  }
  void decode_payload(CephContext *cct) {
    bufferlist::iterator p = payload.begin();
    ::decode(epoch, p);
    ::decode(pg_list, p);
    if (header.version > 1) {
      ::decode(query_epoch, p);
    }
  }
  void print(ostream& out) {
    out << "pg_notify(";
    for (vector<PG::Info>::iterator i = pg_list.begin();
         i != pg_list.end();
         ++i) {
      if (i != pg_list.begin())
	out << ",";
      out << i->pgid;
    }
    out << " epoch " << epoch
	<< " query_epoch " << query_epoch
	<< ")";
  }
};

#endif
