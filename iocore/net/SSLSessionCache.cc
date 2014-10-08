/** @file

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#include <cstring>
#include <deque>
#include "P_SSLConfig.h"
#include "SSLSessionCache.h"

#define SSLSESSIONCACHE_STRINGIFY0(x) #x
#define SSLSESSIONCACHE_STRINGIFY(x) SSLSESSIONCACHE_STRINGIFY0(x)
#define SSLSESSIONCACHE_LINENO SSLSESSIONCACHE_STRINGIFY(__LINE__)

#ifdef DEBUG
#define PRINT_BUCKET(x) this->print(x " at " __FILE__ ":" SSLSESSIONCACHE_LINENO);
#else
#define PRINT_BUCKET(x)
#endif

using ts::detail::RBNode;

/* Session Cache */
SSLSessionCache::SSLSessionCache()
  : session_bucket(NULL) {
  Debug("ssl.session_cache", "Created new ssl session cache %p with %ld buckets each with size max size %ld", this, SSLConfigParams::session_cache_number_buckets, SSLConfigParams::session_cache_max_bucket_size);

  session_bucket = new SSLSessionBucket[SSLConfigParams::session_cache_number_buckets];
}

SSLSessionCache::~SSLSessionCache() {
  delete []session_bucket;
}

bool SSLSessionCache::getSession(const SSLSessionID &sid, const char *sni_name, SSL_SESSION **sess) {
  uint64_t hash = sid.hash();
  uint64_t target_bucket = hash % SSLConfigParams::session_cache_number_buckets;
  SSLSessionBucket *bucket = &session_bucket[target_bucket];
  bool ret = false;

  if (is_debug_tag_set("ssl.session_cache")) {
     char buf[sid.len * 2 + 1];
     sid.toString(buf, sizeof(buf));
     Debug("ssl.session_cache.get", "SessionCache looking in bucket %" PRId64 " (%p) for session '%s' (hash: %" PRIX64 ").", target_bucket, bucket, buf, hash);
   }

  ret = bucket->getSession(sid, sni_name, sess);

  if (ret)
    SSL_INCREMENT_DYN_STAT(ssl_session_cache_hit);
  else
    SSL_INCREMENT_DYN_STAT(ssl_session_cache_miss);

  return ret;
}

void SSLSessionCache::removeSession(const SSLSessionID &sid) {
  uint64_t hash = sid.hash();
  uint64_t target_bucket = hash % SSLConfigParams::session_cache_number_buckets;
  SSLSessionBucket *bucket = &session_bucket[target_bucket];

  if (is_debug_tag_set("ssl.session_cache")) {
     char buf[sid.len * 2 + 1];
     sid.toString(buf, sizeof(buf));
     Debug("ssl.session_cache.remove", "SessionCache using bucket %" PRId64 " (%p): Removing session '%s' (hash: %" PRIX64 ").", target_bucket, bucket, buf, hash);
   }

  SSL_INCREMENT_DYN_STAT(ssl_session_cache_eviction);
  bucket->removeSession(sid);
}

void SSLSessionCache::insertSession(const SSLSessionID &sid, const char *sni_name, SSL_SESSION *sess) {
  uint64_t hash = sid.hash();
  uint64_t target_bucket = hash % SSLConfigParams::session_cache_number_buckets;
  SSLSessionBucket *bucket = &session_bucket[target_bucket];

  if (is_debug_tag_set("ssl.session_cache")) {
     char buf[sid.len * 2 + 1];
     sid.toString(buf, sizeof(buf));
     Debug("ssl.session_cache.insert", "SessionCache using bucket %" PRId64 " (%p): Inserting session '%s' (hash: %" PRIX64 ").", target_bucket, bucket, buf, hash);
   }

  bucket->insertSession(sid, sni_name, sess);
}

void SSLSessionBucket::insertSession(const SSLSessionID &id, const char *sni_name, SSL_SESSION *sess) {
  size_t len = i2d_SSL_SESSION(sess, NULL); // make sure we're not going to need more than SSL_MAX_SESSION_SIZE bytes
  /* do not cache a session that's too big. */
  if (len > (size_t) SSL_MAX_SESSION_SIZE) {
      Debug("ssl.session_cache", "Unable to save SSL session because size of %" PRId64 " exceeds the max of %d", len, SSL_MAX_SESSION_SIZE);
      return;
  }

  if (is_debug_tag_set("ssl.session_cache")) {
    char buf[id.len * 2 + 1];
    id.toString(buf, sizeof(buf));
    Debug("ssl.session_cache", "Inserting session '%s' to bucket %p with sni name '%s'", buf, this, sni_name);
  }

  Ptr<IOBufferData> buf;
  buf = new_IOBufferData(buffer_size_to_index(len, MAX_BUFFER_SIZE_INDEX), MEMALIGNED);
  ink_release_assert(static_cast<size_t>(buf->block_size()) >= len);
  unsigned char *loc = reinterpret_cast<unsigned char *>(buf->data());
  i2d_SSL_SESSION(sess, &loc);

  SSLSession *ssl_session = new SSLSession(id, sni_name, buf, len);

  ink_scoped_try_mutex scoped_mutex(mutex);
  if (!scoped_mutex.hasLock()) {
    SSL_INCREMENT_DYN_STAT(ssl_session_cache_lock_contention);
    if (SSLConfigParams::session_cache_skip_on_lock_contention)
      return;

    scoped_mutex.lock();
  }

  PRINT_BUCKET("insertSession before")
  if (queue.size >= static_cast<int>(SSLConfigParams::session_cache_max_bucket_size)) {
      removeOldestSession();
  }

  /* do the actual insert */
  queue.enqueue(ssl_session);

  PRINT_BUCKET("insertSession after")
}



bool SSLSessionBucket::getSession(const SSLSessionID &id, const char *sni_name, SSL_SESSION **sess) {
  char buf[id.len * 2 + 1];
  if (is_debug_tag_set("ssl.session_cache")) {
   id.toString(buf, sizeof(buf));
  }

  Debug("ssl.session_cache", "Looking for session with id '%s' in bucket %p with sni name '%s'", buf, this, sni_name);

  ink_scoped_try_mutex scoped_mutex(mutex);
  if (!scoped_mutex.hasLock()) {
   SSL_INCREMENT_DYN_STAT(ssl_session_cache_lock_contention);
   if (SSLConfigParams::session_cache_skip_on_lock_contention)
     return false;
   scoped_mutex.lock();
  }

  PRINT_BUCKET("getSession")

  // We work backwards because that's the most likely place we'll find our session...
  SSLSession *node = queue.tail;
  while (node) {
    if (node->session_id == id)
    {
      if ((node->sni_name == NULL && sni_name == NULL) /* this session doesn't have an associated SNI name */||
         (node->sni_name && sni_name && strcmp(node->sni_name, sni_name) == 0)) { /* the session does have an associated SNI name */
       Debug("ssl.session_cache", "Found session with id '%s' in bucket %p with sni name '%s'.", buf, this, sni_name);

       const unsigned char *loc = reinterpret_cast<const unsigned char *>(node->asn1_data->data());
       *sess = d2i_SSL_SESSION(NULL, &loc, node->len_asn1_data);

       return true;
      } else {
       Debug("ssl.session_cache", "Found session with id '%s' in bucket %p but sni names didn't match! '%s' != '%s'.", buf, this, node->sni_name, sni_name);
       return false;
      }
    }

    node = node->link.prev;
  }

  Debug("ssl.session_cache", "Session with id '%s' not found in bucket %p.", buf, this);
  return false;
}

void inline SSLSessionBucket::print(const char *ref_str) const {
  /* NOTE: This method assumes you're already holding the bucket lock */
  if (!is_debug_tag_set("ssl.session_cache.bucket")) {
     return;
  }

  fprintf(stderr, "-------------- BUCKET %p (%s) ----------------\n", this, ref_str);
  fprintf(stderr, "Current Size: %d, Max Size: %" PRId64 "\n", queue.size, SSLConfigParams::session_cache_max_bucket_size);
  fprintf(stderr, "Queue: \n");

  SSLSession *node = queue.head;
  while(node) {
    char s_buf[2 * node->session_id.len + 1];
    node->session_id.toString(s_buf, sizeof(s_buf));
    fprintf(stderr, "  %s\n", s_buf);
    node = node->link.next;
  }
}

void inline SSLSessionBucket::removeOldestSession() {
  PRINT_BUCKET("removeOldestSession before")
  while (queue.head && queue.size >= static_cast<int>(SSLConfigParams::session_cache_max_bucket_size)) {
    SSLSession *old_head = queue.pop();
    if (is_debug_tag_set("ssl.session_cache")) {
      char buf[old_head->session_id.len * 2 + 1];
      old_head->session_id.toString(buf, sizeof(buf));
      Debug("ssl.session_cache", "Removing session '%s' from bucket %p because the bucket has size %d and max %" PRId64, buf, this, (queue.size + 1), SSLConfigParams::session_cache_max_bucket_size);
    }
    delete old_head;
  }
  PRINT_BUCKET("removeOldestSession after")
}

void SSLSessionBucket::removeSession(const SSLSessionID &id) {
  ink_scoped_mutex scoped_mutex(mutex); // We can't bail on contention here because this session MUST be removed.
  SSLSession *node = queue.head;
  while (node) {
    if (node->session_id == id)
    {
      queue.remove(node);
      delete node;
      return;
    }
  }
}

/* Session Bucket */
SSLSessionBucket::SSLSessionBucket() : root(NULL) {
  Debug("ssl.session_cache", "Created new bucket %p with max size %ld", this, SSLConfigParams::session_cache_max_bucket_size);
	ink_mutex_init(&mutex, "session_bucket");
}

SSLSessionBucket::~SSLSessionBucket() {
	ink_mutex_destroy(&mutex);
}


