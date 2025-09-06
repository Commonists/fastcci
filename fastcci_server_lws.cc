// Minimal libwebsockets-based server for FastCCI
// Build: g++ -std=c++17 -O2 -Wall -Wextra fastcci_server_lws.cc -lwebsockets -lpthread -o fastcci_server_lws

#include <libwebsockets.h>

#include "fastcci.h"   // keeps your result_type, tree_type, rb*, readFile(), compare(), etc.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

// --------------------- Shared FastCCI data (unchanged) ---------------------
pthread_mutex_t handlerMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition = PTHREAD_COND_INITIALIZER;

const int maxdepth = 500;
int maxcat;
tree_type *cat, *tree, *parent;
time_t treetime;

struct resultList {
  int max, num;
  result_type * buf;
  unsigned char *mask, *tags;
  resultList(int initialSize = 1024 * 1024) : max(initialSize), num(0), tags(NULL) {
    buf = (result_type *)malloc(max * sizeof *buf);
    mask = (unsigned char *)malloc(maxcat * sizeof *mask);
    if (buf == NULL || mask == NULL) { perror("resultList()"); exit(1); }
  }
  result_type * tail() const { return &(buf[num]); }
  void grow(int len) {
    if (num + len > max) {
      while (num + len > max) max *= 2;
      if ((buf = (result_type *)realloc(buf, max * sizeof *buf)) == NULL) { perror("resultList->grow()"); exit(1); }
    }
  }
  void shrink() {
    if (num < max / 2 && max > (1024 * 1024)) {
      max /= 2;
      if ((buf = (result_type *)realloc(buf, max * sizeof *buf)) == NULL) { perror("resultList->shrink()"); exit(1); }
    }
  }
  void clear() { memset(mask, 0, maxcat * sizeof *mask); }
  void addTags() {
    if ((tags = (unsigned char *)calloc(maxcat, sizeof(*tags))) == NULL) { perror("resultList->addTags()"); exit(1); }
  }
  void sort() { qsort(buf, num, sizeof *buf, compare); }
};

resultList *result[2], *goodImages;
struct ringBuffer rb;

inline bool isCategory(int i){ return (i >= 0 && i < maxcat && cat[i] >= 0); }
inline bool isFile(int i){ return (i >= 0 && i < maxcat && cat[i] < 0); }

struct WorkItem {
  // request
  int c1 = -1, c2 = -1;
  int d1 = -1, d2 = -1;
  int o = 0, s = 100;
  wiType type = WT_INTERSECT;

  // live status
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t  cond  = PTHREAD_COND_INITIALIZER;
  wiStatus status = WS_WAITING;

  // connection kind + backpointer (set by server layer)
  wiConn connection = WC_XHR;

  // For HTTP streaming
  struct pss_http *ph = nullptr;
  // For WS streaming
  struct pss_ws *pw = nullptr;
};

// Global queue (kept as ring semantics like original)
int aItem = 0, bItem = 0;
const int maxItem = 1000;
static WorkItem queueRing[maxItem];

// --------------------- Small “emit” helpers (HTTP/WS bridge) ----------------
struct Accum { std::string buf; int count = 0; static constexpr int maxqueue = 50; };

static void http_emit_line(struct pss_http *ph, const std::string &s);
static void ws_emit_line(struct pss_ws *pw, const std::string &s);

static inline void emit_line(WorkItem &wi, const std::string &s){
  if (wi.connection == WC_SOCKET) ws_emit_line(wi.pw, s + "\n");
  else                            http_emit_line(wi.ph, s + "\n");
}
static inline void result_start(WorkItem &wi){ emit_line(wi, "COMPUTE_START"); }
static inline void result_done(WorkItem &wi){ emit_line(wi, "DONE"); }
static void result_printf(WorkItem &wi, const char *fmt, ...){
  char buf[512];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  emit_line(wi, std::string(buf));
}
static void result_flush(WorkItem &wi, Accum &acc){
  if (!acc.buf.empty()){
    emit_line(wi, std::string("RESULT ") + acc.buf.substr(0, acc.buf.size()-1));
    acc.buf.clear(); acc.count = 0;
  }
}
static void result_queue(WorkItem &wi, Accum &acc, result_type item, unsigned char tag){
  char tmp[64];
  snprintf(tmp, sizeof(tmp), "%d,%d,%d|",
           int(item & cat_mask), int((item & depth_mask) >> depth_shift), tag);
  acc.buf.append(tmp);
  if (++acc.count == Accum::maxqueue) result_flush(wi, acc);
}

// --------------------- Original compute helpers (unchanged) -----------------
result_type history[maxdepth];

static void fetchFiles_emit_cancelable(tree_type id, int depth, resultList *r1, std::atomic_bool *cancel){
  rbClear(rb); rbPush(rb, id);
  result_type r, d, e, i; unsigned char f; int c, len;
  while (!rbEmpty(rb)){
    if (cancel && cancel->load()) break;
    r = rbPop(rb);
    d = (r & depth_mask) >> depth_shift;
    i = r & cat_mask;
    if (i >= maxcat) continue;
    r1->mask[i] = 1;
    int c = cat[i], cend = tree[c], cfile = tree[c + 1];
    c += 2;
    if (d < depth || depth < 0){
      e = (d + 1) << depth_shift;
      while (c < cend){
        if (cancel && cancel->load()) break;
        if (tree[c] < maxcat && r1->mask[tree[c]] == 0 && cat[tree[c]] > 0)
          rbPush(rb, tree[c] | e);
        c++;
      }
    }
    int len = cfile - c; r1->grow(len);
    result_type *dst = r1->tail(), *old = dst; tree_type * src = &(tree[c]);
    f = d < 254 ? (d + 1) : 255;
    d = d << depth_shift;
    while (len--){
      if (cancel && cancel->load()) break;
      r = (*src++);
      if ((r & cat_mask) < maxcat && r1->mask[r & cat_mask] == 0){
        *dst++ = (r | d);
        r1->mask[r & cat_mask] = f;
      }
    }
    r1->num += dst - old;
  }
}

static void tagCat_emit(tree_type sid, tree_type did, int maxDepth, resultList * r1, WorkItem &wi){
  rbClear(rb);
  bool c2isFile = (cat[did] < 0);
  int depth = -1; result_type id = sid;
  rbPush(rb, sid);
  result_type r, d, e, ld = -1; int c;
  bool foundPath = false;
  while (!rbEmpty(rb) && !foundPath){
    r = rbPop(rb); d = (r & depth_mask) >> depth_shift; id = r & cat_mask;
    if (d != ld){ depth++; ld = d; }
    int c = cat[id], cend = tree[c], cend2 = tree[c + 1]; c += 2;
    if (depth < maxDepth || maxDepth < 0){
      e = (d + 1) << depth_shift;
      while (c < cend){
        if (tree[c] == did){ parent[did] = id; id = did; foundPath = true; break; }
        if (tree[c] < maxcat && r1->mask[tree[c]] == 0){ parent[tree[c]] = id; r1->mask[tree[c]] = 1; rbPush(rb, tree[c] | e); }
        c++;
      }
    }
    if (c2isFile){ for (c = cend; c < cend2; ++c) if (tree[c] == did){ foundPath = true; break; } }
  }
  Accum acc;
  if (foundPath){
    int i = 0; while (true){ history[i++] = id; if (id == sid) break; id = parent[id]; }
    int len = i; while (i--) result_queue(wi, acc, history[i] + (result_type(len - i) << depth_shift), 0);
    result_flush(wi, acc);
  } else {
    emit_line(wi, "NOPATH");
  }
}

static void traverse_emit(int outstart, int outsize, resultList *r1, WorkItem &wi){
  Accum acc; int outend = outstart + outsize; if (outend > r1->num) outend = r1->num;
  for (int i = outstart; i < outend; ++i){
    result_type r = r1->buf[i] & cat_mask;
    result_queue(wi, acc, r1->buf[i], r1->tags == NULL ? goodImages->tags[r] : r1->tags[r]);
  }
  result_flush(wi, acc);
  result_printf(wi, "OUTOF %d", r1->num);
}

static void notin_emit(int outstart, int outsize, resultList *r1, resultList *r2, WorkItem &wi){
  Accum acc; int n = 0, i; int outend = outstart + outsize; result_type r;
  for (i = 0; i < r1->num; ++i){
    r = r1->buf[i] & cat_mask;
    if (r < maxcat && r2->mask[r] == 0){
      n++; if (n <= outstart) continue;
      result_queue(wi, acc, r1->buf[i], r1->tags == NULL ? goodImages->tags[r] : r1->tags[r]);
      if (n >= outend) break;
    }
  }
  result_flush(wi, acc);
  if (i == r1->num) result_printf(wi, "OUTOF %d", n - outstart);
  else if (i > 0) result_printf(wi, "OUTOF %d", (outend * r1->num) / i);
}

static void intersect_emit(int outstart, int outsize, resultList *r1, resultList *r2, WorkItem &wi){
  Accum acc; int n = 0; int outend = outstart + outsize; if (r2->num == 0){ result_printf(wi, "OUTOF %d", 0); return; }
  for (int i = 0; i < r1->num; ++i){
    result_type r = r1->buf[i] & cat_mask; if (r >= maxcat) continue;
    result_type m = r2->mask[r]; if (m != 0){
      n++; if (n <= outstart) continue;
      result_queue(wi, acc, r1->buf[i] + ((m - 1) << depth_shift), r2->tags == NULL ? goodImages->tags[r] : r2->tags[r]);
      if (n >= outend) break;
    }
  }
  result_flush(wi, acc);
  result_printf(wi, "OUTOF %d", n - outstart);
}

static void findFQV_emit(int outstart, int outsize, resultList *r1, WorkItem &wi){
  if (r1->num == 0){ result_printf(wi, "OUTOF %d", 0); return; }
  Accum acc; int n = 0; unsigned char k; int i, outend = outstart + outsize; result_type r, m;
  for (k = 1; k <= 4; ++k){
    for (i = 0; i < r1->num; ++i){
      r = r1->buf[i] & cat_mask; if (r >= maxcat) continue;
      m = goodImages->mask[r];
      if (m != 0 && k == goodImages->tags[r]){
        n++; if (n <= outstart) continue;
        result_queue(wi, acc, r1->buf[i] + ((m - 1) << depth_shift), goodImages->tags[r]);
        if (n >= outend) break;
      }
    }
    if (n >= outend) break;
  }
  result_flush(wi, acc);
  if (k == 5) result_printf(wi, "OUTOF %d", n - outstart);
  else if (((k - 1) * r1->num + i) > 0) result_printf(wi, "OUTOF %d", (outend * r1->num * 3) / ((k - 1) * r1->num + i));
}

// --------------------- Global worker thread (unchanged logic) --------------
static std::atomic_bool g_stop{false};

static void computeThread(){
  while (!g_stop.load()){
    // wait for pthread condition
    pthread_mutex_lock(&mutex);
    while (aItem == bItem) pthread_cond_wait(&condition, &mutex);
    pthread_mutex_unlock(&mutex);

    while (bItem > aItem){
      int idx = aItem % maxItem;
      WorkItem &wi = queueRing[idx];

      result_start(wi);

      pthread_mutex_lock(&wi.mutex);
      wi.status = WS_PREPROCESS;
      pthread_cond_signal(&wi.cond);
      pthread_mutex_unlock(&wi.mutex);

      int nr = 0;
      if (wi.type == WT_PATH){
        result[0]->clear();
        pthread_mutex_lock(&wi.mutex);
        wi.status = WS_STREAMING;
        pthread_cond_signal(&wi.cond);
        pthread_mutex_unlock(&wi.mutex);
        tagCat_emit((tree_type)wi.c1, (tree_type)wi.c2, wi.d1, result[0], wi);
      } else {
        result[0]->num = 0;
        result[1]->num = 0;
        pthread_mutex_lock(&wi.mutex);
        wi.status = WS_PREPROCESS;
        pthread_cond_signal(&wi.cond);
        pthread_mutex_unlock(&wi.mutex);

        int cid[2] = {wi.c1, wi.c2};
        int depth[2] = {wi.d1, wi.d2};
        nr = (wi.type == WT_TRAVERSE || wi.type == WT_FQV) ? 1 : 2;

        for (int j = 0; j < nr; ++j){
          result[j]->clear();
          fetchFiles_emit_cancelable(cid[j], depth[j], result[j], nullptr);
        }

        pthread_mutex_lock(&wi.mutex);
        wi.status = WS_COMPUTING;
        pthread_cond_signal(&wi.cond);
        pthread_mutex_unlock(&wi.mutex);

        switch (wi.type){
          case WT_TRAVERSE:  traverse_emit(wi.o, wi.s, result[0], wi); break;
          case WT_FQV:       findFQV_emit(wi.o, wi.s, result[0], wi); break;
          case WT_NOTIN:     notin_emit(wi.o, wi.s, result[0], result[1], wi); break;
          case WT_INTERSECT: intersect_emit(wi.o, wi.s, result[0], result[1], wi); break;
          case WT_PATH: break;
        }
      }
      time_t now = time(NULL);
      result_printf(wi, "DBAGE %.f", difftime(now, treetime));

      result_done(wi);
      pthread_mutex_lock(&wi.mutex);
      wi.status = WS_DONE;
      pthread_cond_signal(&wi.cond);
      pthread_mutex_unlock(&wi.mutex);

      for (int j = 0; j < nr; ++j) result[j]->shrink();

      pthread_mutex_lock(&mutex);
      aItem++;
      pthread_mutex_unlock(&mutex);
    }
  }
}

// --------------------- LWS plumbing (HTTP + WS) ----------------------------

// Per-HTTP-connection state
struct pss_http {
  lws *wsi = nullptr;
  bool js = false;
  // outgoing queue of lines to send (heap-allocated: LWS does not run C++ ctors)
  std::mutex *qmtx = nullptr;
  std::deque<std::string> *q = nullptr;
  // partial write
  std::string pend;
  size_t sent = 0;
  // back to work item
  WorkItem *wi = nullptr;
};

// Per-WS-connection state
struct msg { char *data; size_t len; };
static void destroy_msg(void *p){ auto *m = (msg*)p; if (m && m->data) free(m->data); }
struct pss_ws {
  lws *wsi = nullptr;
  lws_ring *ring = nullptr;
  uint32_t tail = 0;
  WorkItem *wi = nullptr;
  // parsed params (pre-upgrade)
  int c1=-1,c2=-1,d1=-1,d2=-1,o=0,s=100; std::string a;
};

static inline void http_queue(pss_http *ph, const std::string &s){
  if (!ph || !ph->qmtx || !ph->q) return;
  std::lock_guard<std::mutex> lk(*(ph->qmtx));
  ph->q->push_back(s);
  if (ph->wsi) lws_callback_on_writable(ph->wsi);
}
static inline void http_emit_line(pss_http *ph, const std::string &s){
  if (!ph) return;
  if (ph->js){
    std::string line = s;
    std::string out;
    if (line.rfind("DONE", 0) == 0) out = " 'DONE'] );\n";
    else {
      std::string no_nl = line; if (!no_nl.empty() && no_nl.back()=='\n') no_nl.pop_back();
      out = " '" + no_nl + "',";
    }
    http_queue(ph, out);
  } else {
    http_queue(ph, s);
  }
}
static inline void ws_emit_line(pss_ws *pw, const std::string &s){
  if (!pw || !pw->ring) return;
  msg m; m.len = s.size(); m.data = (char*)malloc(m.len); memcpy(m.data, s.data(), m.len);
  lws_ring_insert(pw->ring, &m, 1);
  if (pw->wsi) lws_callback_on_writable(pw->wsi);
}

// Query parsing helper
static std::string get_arg_http(struct lws *wsi, const char *key){
  char frag[256]; int idx = 0; size_t klen = strlen(key);
  while (lws_hdr_copy_fragment(wsi, frag, (int)sizeof(frag), WSI_TOKEN_HTTP_URI_ARGS, idx) > 0){
    if (!strncmp(frag, key, klen) && frag[klen] == '=') return std::string(frag + klen + 1);
    ++idx;
  }
  return {};
}
static inline int toi(const std::string &s, int defv){ if (s.empty()) return defv; return atoi(s.c_str()); }

// HTTP protocol
static int callback_http(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len){
  auto *pss = (pss_http*)user;
  switch (reason){
    case LWS_CALLBACK_FILTER_HTTP_CONNECTION:
      // Pre-upgrade filter: do not serve anything here; let WS proceed
      return 0;
    case LWS_CALLBACK_HTTP: {
      // If upgrade, let WS protocol handle it
#if defined(lws_http_is_upgrade)
      if (lws_http_is_upgrade(wsi)) return 0;
#endif
      if (lws_hdr_total_length(wsi, WSI_TOKEN_UPGRADE) > 0) return 0;

      // route
      char uri[256]; if (lws_hdr_copy(wsi, uri, sizeof(uri), WSI_TOKEN_GET_URI) <= 0) uri[0] = 0;

      if (strcmp(uri, "/status") == 0){
        // Build simple JSON (like original)
        double la[3] = {0,0,0}; getloadavg(la, 3);
        time_t now = time(NULL);
        int qlen; { pthread_mutex_lock(&mutex); qlen = bItem - aItem; pthread_mutex_unlock(&mutex); }
        char body[256];
        snprintf(body, sizeof(body), "{\"queue\":%d,\"relsize\":%d,\"dbage\":%.f,\"load\":[%f,%f,%f]}",
                 qlen, maxcat, difftime(now, treetime), la[0], la[1], la[2]);
        unsigned char hdr[LWS_PRE + 512]; unsigned char *p = &hdr[LWS_PRE], *e = &hdr[sizeof(hdr)-1];
        if (lws_add_http_common_headers(wsi, 200, "application/json; charset=utf-8", (lws_filepos_t)strlen(body), &p, e)) return 1;
        if (lws_finalize_write_http_header(wsi, &hdr[LWS_PRE], &p, e)) return 1;
        lws_write(wsi, (unsigned char*)body, (unsigned int)strlen(body), LWS_WRITE_HTTP);
        lws_write(wsi, (unsigned char*)"", 0, LWS_WRITE_HTTP_FINAL);
        return -1;
      }

      if (strcmp(uri, "/fastcci-ws") == 0) {
        // explicit WS path alias — do not write HTTP here
        return 0;
      }

      if (uri[0] != 0 && strcmp(uri, "/") != 0){
        const char *body = "Bad request. Use /?c1=...&... or /status.\n";
        unsigned char hdr[LWS_PRE + 512]; unsigned char *p = &hdr[LWS_PRE], *e = &hdr[sizeof(hdr)-1];
        if (lws_add_http_common_headers(wsi, 400, "text/plain; charset=utf-8", (lws_filepos_t)strlen(body), &p, e)) return 1;
        if (lws_finalize_write_http_header(wsi, &hdr[LWS_PRE], &p, e)) return 1;
        lws_write(wsi, (unsigned char*)body, (unsigned int)strlen(body), LWS_WRITE_HTTP);
        lws_write(wsi, (unsigned char*)"", 0, LWS_WRITE_HTTP_FINAL);
        return -1;
      }

      // compute request on root path
      std::string c1s = get_arg_http(wsi, "c1");
      if (c1s.empty()){
        const char *body = "Missing required parameter c1.\n";
        unsigned char hdr[LWS_PRE + 512]; unsigned char *p = &hdr[LWS_PRE], *e = &hdr[sizeof(hdr)-1];
        if (lws_add_http_common_headers(wsi, 400, "text/plain; charset=utf-8", (lws_filepos_t)strlen(body), &p, e)) return 1;
        if (lws_finalize_write_http_header(wsi, &hdr[LWS_PRE], &p, e)) return 1;
        lws_write(wsi, (unsigned char*)body, (unsigned int)strlen(body), LWS_WRITE_HTTP);
        lws_write(wsi, (unsigned char*)"", 0, LWS_WRITE_HTTP_FINAL);
        return -1;
      }

      int idx;
      pthread_mutex_lock(&mutex);
      if (bItem - aItem + 1 >= maxItem){ pthread_mutex_unlock(&mutex); return -1; }
      idx = bItem % maxItem;
      pthread_mutex_unlock(&mutex);

      WorkItem &wi = queueRing[idx];
      wi.c1 = toi(c1s, -1);
      wi.c2 = toi(get_arg_http(wsi, "c2"), wi.c1);
      wi.d1 = toi(get_arg_http(wsi, "d1"), -1);
      wi.d2 = toi(get_arg_http(wsi, "d2"), -1);
      wi.o  = toi(get_arg_http(wsi, "o"), 0);
      wi.s  = toi(get_arg_http(wsi, "s"), 100);
      std::string a = get_arg_http(wsi, "a");
      if      (a == "not")  wi.type = WT_NOTIN;
      else if (a == "fqv")  wi.type = WT_FQV;
      else if (a == "list") wi.type = WT_TRAVERSE;
      else if (a == "path") wi.type = WT_PATH;
      else                  wi.type = WT_INTERSECT;

      // Validate like original
      if (wi.c1 >= maxcat || wi.c2 >= maxcat || wi.c1 < 0 || wi.c2 < 0) return -1;
      if (isFile(wi.c1) || (isFile(wi.c2) && wi.type != WT_PATH)) return -1;
      if (wi.type == WT_PATH && wi.c1 == wi.c2) return -1;

      // response headers: stream (unknown length)
      bool use_js = (get_arg_http(wsi, "t") == "js");
      unsigned char hdr[LWS_PRE + 512]; unsigned char *p = &hdr[LWS_PRE], *e = &hdr[sizeof(hdr)-1];
      const char *ctype = use_js ? "application/javascript; charset=utf-8" : "text/plain; charset=utf-8";
      if (lws_add_http_common_headers(wsi, 200, ctype, (lws_filepos_t)-1, &p, e)) return 1;
      if (lws_finalize_write_http_header(wsi, &hdr[LWS_PRE], &p, e)) return 1;

      // bind HTTP pss + queue item
      pss->wsi = wsi; pss->js = use_js; pss->wi = &wi;
      // allocate per-conn queue + mutex
      pss->qmtx = new std::mutex();
      pss->q = new std::deque<std::string>();
      wi.connection = use_js ? WC_JS : WC_XHR; wi.ph = pss; wi.pw = nullptr;

      // enqueue job
      pthread_mutex_lock(&mutex);
      bItem++;
      pthread_cond_signal(&condition);
      pthread_mutex_unlock(&mutex);

      // initial signals like original: queue position & start callback for JS
      {
        int pos; pthread_mutex_lock(&mutex); pos = (bItem - aItem) - 1; pthread_mutex_unlock(&mutex);
        http_emit_line(pss, std::string("QUEUED ") + std::to_string(pos) + "\n");
        if (use_js) http_queue(pss, "fastcciCallback( [\n");
      }

      lws_callback_on_writable(wsi);
      return 0;
    }
    case LWS_CALLBACK_HTTP_WRITEABLE: {
      // send any pending / queued bytes
      if (!pss) return 0;

      // if a partial exists, continue it
      if (!pss->pend.empty()){
        const char *data = pss->pend.data() + pss->sent;
        size_t left = pss->pend.size() - pss->sent;
        int n = lws_write(wsi, (unsigned char*)data, (unsigned int)left, LWS_WRITE_HTTP);
        if (n < 0) return -1;
        pss->sent += (size_t)n;
        if (pss->sent < pss->pend.size()){ lws_callback_on_writable(wsi); return 0; }
        pss->pend.clear(); pss->sent = 0;
      }

      // pop next line
      std::string out;
      {
        if (pss->qmtx && pss->q){
          std::lock_guard<std::mutex> lk(*(pss->qmtx));
          if (!pss->q->empty()){ out = std::move(pss->q->front()); pss->q->pop_front(); }
        }
      }
      if (!out.empty()){
        pss->pend = std::move(out); pss->sent = 0;
        lws_callback_on_writable(wsi);
        return 0;
      }

      // If the job is done and no more output, finalize HTTP
      if (pss->wi && pss->wi->status == WS_DONE){
        if (pss->js) { // close callback array if JS
          if (pss->qmtx && pss->q) {
            std::lock_guard<std::mutex> lk(*(pss->qmtx));
            if (pss->q->empty()) {
              const char tail[] = " 'DONE'] );\n";
              lws_write(wsi, (unsigned char*)tail, (unsigned int)strlen(tail), LWS_WRITE_HTTP);
            }
          } else {
            // send the JS tail and close
            const char tail[] = " 'DONE'] );\n";
            lws_write(wsi, (unsigned char*)tail, (unsigned int)strlen(tail), LWS_WRITE_HTTP);
          }
        }
        lws_write(wsi, (unsigned char*)"", 0, LWS_WRITE_HTTP_FINAL);
        return -1;
      }

      // otherwise keep waiting
      lws_callback_on_writable(wsi);
      return 0;
    }
    case LWS_CALLBACK_CLOSED_HTTP:
      // free per-conn HTTP resources
      if (pss){
        if (pss->q){ delete pss->q; pss->q = nullptr; }
        if (pss->qmtx){ delete pss->qmtx; pss->qmtx = nullptr; }
      }
      return 0;
    default:
      return 0;
  }
}

// WS protocol
static int callback_ws(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len){
  auto *pss = (pss_ws*)user;
  switch (reason){
    case LWS_CALLBACK_FILTER_HTTP_CONNECTION:
      // Pre-upgrade filter: do not serve anything here; let WS proceed
      return 0;
    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION: {
      // Parse query pre-upgrade to avoid token availability issues after upgrade
      char uri[256]; if (lws_hdr_copy(wsi, uri, sizeof(uri), WSI_TOKEN_GET_URI) <= 0) uri[0]=0;
      const char *q = strchr(uri,'?'); if (!q) q=""; else q++;
      auto getv=[&](const char* key)->std::string{
        size_t klen=strlen(key); const char* p=q;
        while (p && *p){ const char* amp=strchr(p,'&'); size_t len= amp? (size_t)(amp-p): strlen(p);
          if (len>klen && !strncmp(p,key,klen) && p[klen]=='=') return std::string(p+klen+1, len-(klen+1));
          p = amp? amp+1: nullptr; }
        return std::string(); };
      auto toi2=[&](const std::string &s,int d){ if(s.empty()) return d; char *e=nullptr; long v=strtol(s.c_str(),&e,10); if(e&&*e) return d; return (int)v; };
      std::string s;
      s=getv("c1"); pss->c1 = toi2(s,-1);
      s=getv("c2"); pss->c2 = s.empty()? pss->c1 : toi2(s,-1);
      s=getv("d1"); pss->d1 = toi2(s,-1);
      s=getv("d2"); pss->d2 = toi2(s,-1);
      s=getv("o");  pss->o  = toi2(s,0);
      s=getv("s");  pss->s  = toi2(s,100);
      pss->a = getv("a");
      return 0; }
    case LWS_CALLBACK_ESTABLISHED: {
      // parse query
      std::string c1s = get_arg_http(wsi, "c1");
      if (c1s.empty()) return -1;

      int idx;
      pthread_mutex_lock(&mutex);
      if (bItem - aItem + 1 >= maxItem){ pthread_mutex_unlock(&mutex); return -1; }
      idx = bItem % maxItem;
      pthread_mutex_unlock(&mutex);

      WorkItem &wi = queueRing[idx];
      wi.c1 = toi(c1s, -1);
      wi.c2 = toi(get_arg_http(wsi, "c2"), wi.c1);
      wi.d1 = toi(get_arg_http(wsi, "d1"), -1);
      wi.d2 = toi(get_arg_http(wsi, "d2"), -1);
      wi.o  = toi(get_arg_http(wsi, "o"), 0);
      wi.s  = toi(get_arg_http(wsi, "s"), 100);
      std::string a = get_arg_http(wsi, "a");
      if      (a == "not")  wi.type = WT_NOTIN;
      else if (a == "fqv")  wi.type = WT_FQV;
      else if (a == "list") wi.type = WT_TRAVERSE;
      else if (a == "path") wi.type = WT_PATH;
      else                  wi.type = WT_INTERSECT;
      if (wi.c1 >= maxcat || wi.c2 >= maxcat || wi.c1 < 0 || wi.c2 < 0) return -1;
      if (isFile(wi.c1) || (isFile(wi.c2) && wi.type != WT_PATH)) return -1;
      if (wi.type == WT_PATH && wi.c1 == wi.c2) return -1;

      pss->wsi = wsi;
      pss->ring = lws_ring_create(sizeof(msg), 512, destroy_msg);
      pss->tail = 0;
      pss->wi = &wi;
      wi.connection = WC_SOCKET; wi.pw = pss; wi.ph = nullptr;

      // enqueue job
      pthread_mutex_lock(&mutex);
      int pos = (bItem - aItem); // number ahead after we push
      bItem++;
      pthread_cond_signal(&condition);
      pthread_mutex_unlock(&mutex);

      ws_emit_line(pss, std::string("QUEUED ") + std::to_string(pos) + "\n");
      lws_callback_on_writable(wsi);
      return 0;
    }
    case LWS_CALLBACK_SERVER_WRITEABLE: {
      if (!pss || !pss->ring) return 0;
      const msg *pm = (const msg*)lws_ring_get_element(pss->ring, &pss->tail);
      if (!pm) return 0;
      std::vector<unsigned char> buf(LWS_PRE + pm->len);
      memcpy(&buf[LWS_PRE], pm->data, pm->len);
      int n = lws_write(wsi, &buf[LWS_PRE], (unsigned int)pm->len, LWS_WRITE_TEXT);
      if (n < 0) return -1;
      bool is_done = (pm->len >= 4 && !memcmp(pm->data, "DONE", 4));
      lws_ring_consume(pss->ring, &pss->tail, NULL, 1);
      lws_ring_update_oldest_tail(pss->ring, pss->tail);
      if (is_done) return -1;
      if (lws_ring_get_element(pss->ring, &pss->tail)) lws_callback_on_writable(wsi);
      return 0;
    }
    case LWS_CALLBACK_CLOSED:
      if (pss) {
        // detach work item so compute thread stops emitting to a freed pss
        if (pss->wi) {
          WorkItem *wi = pss->wi;
          wi->pw = nullptr;           // drop WS backpointer
          // do not flip connection type; just prevent ws_emit_line
        }
        if (pss->ring) { lws_ring_destroy(pss->ring); pss->ring = nullptr; }
      }
      return 0;
    default:
      return 0;
  }
}

// --------------------- Notify thread (status push like original) -----------
static void notifyThread(){
  timespec ts; ts.tv_sec = 0; ts.tv_nsec = 200 * 1000 * 1000; // 200ms
  while (!g_stop.load()){
    nanosleep(&ts, nullptr);

    pthread_mutex_lock(&handlerMutex);
    pthread_mutex_lock(&mutex);

    for (int i = aItem; i < bItem; ++i){
      WorkItem &wi = queueRing[i % maxItem];
      // wake compute waiters (like original)
      pthread_mutex_lock(&wi.mutex);
      pthread_cond_signal(&wi.cond);
      pthread_mutex_unlock(&wi.mutex);

      // also push status over HTTP/WS streams
      if (wi.connection == WC_SOCKET && wi.pw){
        if (wi.status == WS_WAITING){
          int ahead = (i - aItem);
          ws_emit_line(wi.pw, std::string("WAITING ") + std::to_string(ahead) + "\n");
        } else if (wi.status == WS_PREPROCESS || wi.status == WS_COMPUTING){
          char msgbuf[64]; snprintf(msgbuf, sizeof(msgbuf), "WORKING %d %d\n", result[0]->num, result[1]->num);
          ws_emit_line(wi.pw, std::string(msgbuf));
        }
      } else if ((wi.connection == WC_XHR || wi.connection == WC_JS) && wi.ph){
        if (wi.status == WS_WAITING){
          int ahead = (i - aItem);
          http_emit_line(wi.ph, std::string("WAITING ") + std::to_string(ahead) + "\n");
        } else if (wi.status == WS_PREPROCESS || wi.status == WS_COMPUTING){
          char msgbuf[64]; snprintf(msgbuf, sizeof(msgbuf), "WORKING %d %d\n", result[0]->num, result[1]->num);
          http_emit_line(wi.ph, std::string(msgbuf));
        }
      }
    }

    pthread_mutex_unlock(&mutex);
    pthread_mutex_unlock(&handlerMutex);
  }
}

// --------------------- main() ----------------------------------------------
int main(int argc, char *argv[]){
  if (argc != 3){
    fprintf(stderr, "%s PORT DATADIR\n", argv[0]);
    return 1;
  }

  // ring buffer init
  rbInit(rb);

  // load DB
  const int buflen = 1000; char fname[buflen];
  snprintf(fname, buflen, "%s/fastcci.cat", argv[2]); unsigned int cat_file_len = readFile(fname, cat); maxcat = cat_file_len / sizeof(tree_type);
  result[0] = new resultList(1024 * 1024);
  result[1] = new resultList(1024 * 1024);
  goodImages = new resultList(512);
  if ((parent = (tree_type *)malloc(maxcat * sizeof *parent)) == NULL){ perror("parent"); exit(1); }
  snprintf(fname, buflen, "%s/fastcci.tree", argv[2]); unsigned int tree_file_len = readFile(fname, tree);
  struct stat statbuf; if (stat(fname, &statbuf) == -1) { perror(fname); exit(1); } treetime = statbuf.st_mtime;

  // precompute goodImages
  int goodCats[][3] = {{3943817,0,1},{5799448,1,1},{91039287,0,2},{3618826,0,3},{4143367,0,4}};
  goodImages->clear(); goodImages->addTags(); result_type r;
  for (int i = 5; i > 0; --i){
    result[0]->clear(); result[0]->num = 0; fetchFiles_emit_cancelable(goodCats[i-1][0], goodCats[i-1][1], result[0], nullptr);
    for (int j = 0; j < result[0]->num; j++){
      r = result[0]->buf[j] & cat_mask; if (r < maxcat){ goodImages->mask[r] = result[0]->mask[r]; goodImages->tags[r] = goodCats[i-1][2]; }
    }
  }
  goodImages->num = -1;

  // threads
  pthread_t th_compute, th_notify;
  if (pthread_create(&th_compute, nullptr, (void*(*)(void*))[](void*)->void*{ computeThread(); return nullptr; }, nullptr)) return 1;
  if (pthread_create(&th_notify,  nullptr, (void*(*)(void*))[](void*)->void*{ notifyThread();  return nullptr; }, nullptr)) return 1;

  // LWS context
  lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE, nullptr);

  lws_context_creation_info info{};
  info.port = atoi(argv[1]);
  static const struct lws_protocols protocols[] = {
    { "http", callback_http, sizeof(pss_http), 0 },
    { "fastcci-ws", callback_ws, sizeof(pss_ws),  0 },
    { nullptr, nullptr, 0, 0 }
  };
  info.protocols = protocols;
  // Keep options minimal to avoid interfering with WS upgrade
  // info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;
  info.timeout_secs = 3600;

  lws_context *context = lws_create_context(&info);
  if (!context){ fprintf(stderr, "Failed to create LWS context\n"); return 1; }

  fprintf(stderr, "Server ready. [%ld,%ld]\n", (long)sizeof(tree_type), (long)sizeof(result_type));

  while (lws_service(context, 0) >= 0) { /* event loop */ }

  g_stop.store(true);
  lws_context_destroy(context);
  munmap(cat, cat_file_len);
  munmap(tree, tree_file_len);
  return 0;
}
