// libwebsockets-based server for FastCCI
#include <libwebsockets.h>

#include "fastcci.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <ctime>
#include <condition_variable>

// --------------------- Shared FastCCI data ---------------------
// thread management objects (only used inside compute helpers; no global onion)
pthread_mutex_t handlerMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition = PTHREAD_COND_INITIALIZER;

// category data and traversal information
const int maxdepth = 500;
int maxcat;
tree_type *cat, *tree, *parent;

// modification time of the tree database file
time_t treetime;

// new result data structure (copied from original server)
struct resultList {
  int max, num;
  result_type * buf;
  unsigned char *mask, *tags;
  resultList(int initialSize = 1024 * 1024) : max(initialSize), num(0), tags(NULL) {
    buf = (result_type *)malloc(max * sizeof *buf);
    mask = (unsigned char *)malloc(maxcat * sizeof *mask);
    if (buf == NULL || mask == NULL) {
      perror("resultList()");
      exit(1);
    }
  }
  result_type * tail() const { return &(buf[num]); }
  void grow(int len) {
    if (num + len > max) {
      while (num + len > max) max *= 2;
      if ((buf = (result_type *)realloc(buf, max * sizeof *buf)) == NULL) {
        perror("resultList->grow()");
        exit(1);
      }
    }
  }
  void shrink() {
    if (num < max / 2 && max > (1024 * 1024)) {
      max /= 2;
      if ((buf = (result_type *)realloc(buf, max * sizeof *buf)) == NULL) {
        perror("resultList->shrink()");
        exit(1);
      }
    }
  }
  void clear() { memset(mask, 0, maxcat * sizeof *mask); }
  void addTags() {
    if ((tags = (unsigned char *)calloc(maxcat, sizeof(*tags))) == NULL) {
      perror("resultList->addTags()");
      exit(1);
    }
  }
  void sort() { qsort(buf, num, sizeof *buf, compare); }
};

resultList *result[2], *goodImages;

// breadth first search ringbuffer
struct ringBuffer rb;

// helpers
inline bool isCategory(int i) { return (i >= 0 && i < maxcat && cat[i] >= 0); }
inline bool isFile(int i) { return (i >= 0 && i < maxcat && cat[i] < 0); }

// --------------------- Query parsing ---------------------
struct Query {
  int64_t c1 = -1, c2 = -1;
  int64_t d1 = -1, d2 = -1;
  int o = 0, s = 100;
  std::string a = "and"; // and|not|fqv|list|path
};
static int64_t to_i64(const char* s) { return s ? strtoll(s, nullptr, 10) : -1; }
static std::string get_arg(struct lws *wsi, const char *key) {
  char frag[256];
  const size_t klen = strlen(key);
  int idx = 0;
  while (lws_hdr_copy_fragment(wsi, frag, (int)sizeof(frag), WSI_TOKEN_HTTP_URI_ARGS, idx) > 0) {
    if (!strncmp(frag, key, klen) && frag[klen] == '=')
      return std::string(frag + klen + 1);
    ++idx;
  }
  return {};
}
static Query parse_query(struct lws *wsi) {
  Query q;
  q.c1 = to_i64(get_arg(wsi, "c1").c_str());
  q.c2 = to_i64(get_arg(wsi, "c2").c_str());
  q.d1 = to_i64(get_arg(wsi, "d1").c_str());
  q.d2 = to_i64(get_arg(wsi, "d2").c_str());
  auto a = get_arg(wsi, "a"); if (!a.empty()) q.a = a;
  auto o = get_arg(wsi, "o"); if (!o.empty()) q.o = atoi(o.c_str());
  auto s = get_arg(wsi, "s"); if (!s.empty()) q.s = atoi(s.c_str());
  return q;
}

// --------------------- Emit helpers ---------------------
struct Accum {
  std::string buf;
  int count = 0;
  static constexpr int maxqueue = 50;
};
static void emit_line(const std::function<void(const std::string&)>& emit, const std::string &s){
  emit(s + "\n");
}
static void result_start(const std::function<void(const std::string&)>& emit){ emit_line(emit, "COMPUTE_START"); }
static void result_done(const std::function<void(const std::string&)>& emit){ emit_line(emit, "DONE"); }
static void result_printf(const std::function<void(const std::string&)>& emit, const char *fmt, ...){
  char buf[512];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  emit_line(emit, std::string(buf));
}
static void result_flush(const std::function<void(const std::string&)>& emit, Accum &acc){
  if (!acc.buf.empty()){
    emit_line(emit, std::string("RESULT ") + acc.buf.substr(0, acc.buf.size()-1));
    acc.buf.clear();
    acc.count = 0;
  }
}
static void result_queue(const std::function<void(const std::string&)>& emit, Accum &acc,
                         result_type item, unsigned char tag){
  char tmp[64];
  snprintf(tmp, sizeof(tmp), "%d,%d,%d|",
           int(item & cat_mask), int((item & depth_mask) >> depth_shift), tag);
  acc.buf.append(tmp);
  if (++acc.count == Accum::maxqueue) result_flush(emit, acc);
}

// --------------------- Core compute (reused) ---------------------
void fetchFiles(tree_type id, int depth, resultList * r1){
  rbClear(rb); rbPush(rb, id);
  result_type r, d, e, i; unsigned char f; int c, len;
  while (!rbEmpty(rb)){
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
      r = (*src++);
      if ((r & cat_mask) < maxcat && r1->mask[r & cat_mask] == 0){
        *dst++ = (r | d);
        r1->mask[r & cat_mask] = f;
      }
    }
    r1->num += dst - old;
  }
}

// path-tagging and emit
result_type history[maxdepth];
static void tagCat_emit(tree_type sid, tree_type did, int maxDepth, resultList * r1,
                        const std::function<void(const std::string&)>& emit){
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
    int len = i; while (i--) result_queue(emit, acc, history[i] + (result_type(len - i) << depth_shift), 0);
    result_flush(emit, acc);
  } else {
    emit_line(emit, "NOPATH");
  }
}

static void traverse_emit(int outstart, int outsize, resultList *r1,
                          const std::function<void(const std::string&)>& emit){
  Accum acc; int outend = outstart + outsize; if (outend > r1->num) outend = r1->num;
  for (int i = outstart; i < outend; ++i){
    result_type r = r1->buf[i] & cat_mask;
    result_queue(emit, acc, r1->buf[i], r1->tags == NULL ? goodImages->tags[r] : r1->tags[r]);
  }
  result_flush(emit, acc);
  result_printf(emit, "OUTOF %d", r1->num);
}

static void notin_emit(int outstart, int outsize, resultList *r1, resultList *r2,
                       const std::function<void(const std::string&)>& emit){
  Accum acc; int n = 0, i; int outend = outstart + outsize; result_type r;
  for (i = 0; i < r1->num; ++i){
    r = r1->buf[i] & cat_mask;
    if (r < maxcat && r2->mask[r] == 0){
      n++; if (n <= outstart) continue;
      result_queue(emit, acc, r1->buf[i], r1->tags == NULL ? goodImages->tags[r] : r1->tags[r]);
      if (n >= outend) break;
    }
  }
  result_flush(emit, acc);
  if (i == r1->num) result_printf(emit, "OUTOF %d", n - outstart);
  else if (i > 0) result_printf(emit, "OUTOF %d", (outend * r1->num) / i);
}

static void intersect_emit(int outstart, int outsize, resultList *r1, resultList *r2,
                           const std::function<void(const std::string&)>& emit){
  Accum acc; int n = 0; int outend = outstart + outsize; if (r2->num == 0){ result_printf(emit, "OUTOF %d", 0); return; }
  for (int i = 0; i < r1->num; ++i){
    result_type r = r1->buf[i] & cat_mask; if (r >= maxcat) continue;
    result_type m = r2->mask[r]; if (m != 0){
      n++; if (n <= outstart) continue;
      result_queue(emit, acc, r1->buf[i] + ((m - 1) << depth_shift), r2->tags == NULL ? goodImages->tags[r] : r2->tags[r]);
      if (n >= outend) break;
    }
  }
  result_flush(emit, acc);
  result_printf(emit, "OUTOF %d", n - outstart);
}

static void findFQV_emit(int outstart, int outsize, resultList *r1,
                         const std::function<void(const std::string&)>& emit){
  if (r1->num == 0){ result_printf(emit, "OUTOF %d", 0); return; }
  Accum acc; int n = 0; unsigned char k; int i, outend = outstart + outsize; result_type r, m;
  for (k = 1; k <= 4; ++k){
    for (i = 0; i < r1->num; ++i){
      r = r1->buf[i] & cat_mask; if (r >= maxcat) continue;
      m = goodImages->mask[r];
      if (m != 0 && k == goodImages->tags[r]){
        n++; if (n <= outstart) continue;
        result_queue(emit, acc, r1->buf[i] + ((m - 1) << depth_shift), goodImages->tags[r]);
        if (n >= outend) break;
      }
    }
    if (n >= outend) break;
  }
  result_flush(emit, acc);
  if (k == 5) result_printf(emit, "OUTOF %d", n - outstart);
  else if (((k - 1) * r1->num + i) > 0) result_printf(emit, "OUTOF %d", (outend * r1->num * 3) / ((k - 1) * r1->num + i));
}

// --------------------- Queue & worker ---------------------
enum ConnType { CT_HTTP, CT_HTTP_JS, CT_WS };
enum JobStatus { JS_WAITING, JS_PREPROCESS, JS_COMPUTING, JS_STREAMING, JS_DONE };

struct WorkItem;

// HTTP per-conn state
struct pss_http { std::deque<std::string> q; std::mutex qmtx; std::atomic_bool closed{false}; WorkItem *wi{nullptr}; lws *wsi{nullptr}; bool js{false}; bool js_header_sent{false}; };

// WS per-conn state
struct msg { char *data; size_t len; };
static void destroy_msg(void *p){ auto *m = (msg*)p; if (m && m->data) free(m->data); }
struct pss_ws { lws_ring *ring = nullptr; uint32_t tail = 0; std::atomic_bool closed{false}; WorkItem *wi{nullptr}; lws *wsi{nullptr}; };

struct WorkItem {
  Query q;
  ConnType ct;
  JobStatus status{JS_WAITING};
  pss_http *ph{nullptr};
  pss_ws *pw{nullptr};
};

static std::mutex gq_mtx;
static std::condition_variable gq_cv;
static std::deque<WorkItem*> gqueue;
static WorkItem *g_current = nullptr; // item being processed

static void emit_to_http(pss_http *ph, const std::string &line){
  if (!ph || ph->closed.load()) return;
  std::string s = line;
  { std::lock_guard<std::mutex> lk(ph->qmtx); ph->q.push_back(s); }
  if (ph->wsi) lws_callback_on_writable(ph->wsi);
}
static void emit_to_ws(pss_ws *pw, const std::string &line){
  if (!pw || pw->closed.load() || !pw->ring) return;
  msg m; m.len = line.size(); m.data = (char*)malloc(m.len); memcpy(m.data, line.data(), m.len);
  lws_ring_insert(pw->ring, &m, 1);
  if (pw->wsi) lws_callback_on_writable(pw->wsi);
}
static void emit_item(WorkItem *wi, const std::string &line){
  if (!wi) return; if (wi->ct == CT_WS) emit_to_ws(wi->pw, line); else emit_to_http(wi->ph, line);
}

static void job_result_start(WorkItem *wi){ emit_item(wi, "COMPUTE_START\n"); }
static void job_result_done(WorkItem *wi){ emit_item(wi, "DONE\n"); }
static void job_result_printf(WorkItem *wi, const char *fmt, ...){ char buf[512]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap); emit_item(wi, std::string(buf) + "\n"); }
static void job_result_flush(WorkItem *wi, Accum &acc){ if (!acc.buf.empty()){ emit_item(wi, std::string("RESULT ") + acc.buf.substr(0, acc.buf.size()-1) + "\n"); acc.buf.clear(); acc.count = 0; } }
static void job_result_queue(WorkItem *wi, Accum &acc, result_type item, unsigned char tag){ char tmp[64]; snprintf(tmp, sizeof(tmp), "%d,%d,%d|", int(item & cat_mask), int((item & depth_mask) >> depth_shift), tag); acc.buf.append(tmp); if (++acc.count == Accum::maxqueue) job_result_flush(wi, acc); }

static void tagCat_emit_item(tree_type sid, tree_type did, int maxDepth, resultList * r1, WorkItem *wi){
  rbClear(rb); bool c2isFile = (cat[did] < 0); int depth = -1; result_type id = sid; rbPush(rb, sid);
  result_type r, d, e, ld = -1; int c; bool foundPath = false;
  while (!rbEmpty(rb) && !foundPath){ r = rbPop(rb); d = (r & depth_mask) >> depth_shift; id = r & cat_mask; if (d != ld){ depth++; ld = d; }
    int c = cat[id], cend = tree[c], cend2 = tree[c + 1]; c += 2; if (depth < maxDepth || maxDepth < 0){ e = (d + 1) << depth_shift; while (c < cend){ if (tree[c] == did){ parent[did] = id; id = did; foundPath = true; break; } if (tree[c] < maxcat && r1->mask[tree[c]] == 0){ parent[tree[c]] = id; r1->mask[tree[c]] = 1; rbPush(rb, tree[c] | e);} c++; } }
    if (c2isFile){ for (c = cend; c < cend2; ++c) if (tree[c] == did){ foundPath = true; break; } } }
  Accum acc; if (foundPath){ int i = 0; while (true){ history[i++] = id; if (id == sid) break; id = parent[id]; } int len = i; while (i--) job_result_queue(wi, acc, history[i] + (result_type(len - i) << depth_shift), 0); job_result_flush(wi, acc); } else { emit_item(wi, "NOPATH\n"); }
}

static void traverse_emit_item(WorkItem *wi, int outstart, int outsize, resultList *r1){ Accum acc; int outend = outstart + outsize; if (outend > r1->num) outend = r1->num; for (int i = outstart; i < outend; ++i){ result_type r = r1->buf[i] & cat_mask; job_result_queue(wi, acc, r1->buf[i], r1->tags == NULL ? goodImages->tags[r] : r1->tags[r]); } job_result_flush(wi, acc); job_result_printf(wi, "OUTOF %d", r1->num); }
static void notin_emit_item(WorkItem *wi, int outstart, int outsize, resultList *r1, resultList *r2){ Accum acc; int n = 0, i; int outend = outstart + outsize; result_type r; for (i = 0; i < r1->num; ++i){ r = r1->buf[i] & cat_mask; if (r < maxcat && r2->mask[r] == 0){ n++; if (n <= outstart) continue; job_result_queue(wi, acc, r1->buf[i], r1->tags == NULL ? goodImages->tags[r] : r1->tags[r]); if (n >= outend) break; } } job_result_flush(wi, acc); if (i == r1->num) job_result_printf(wi, "OUTOF %d", n - outstart); else if (i > 0) job_result_printf(wi, "OUTOF %d", (outend * r1->num) / i); }
static void intersect_emit_item(WorkItem *wi, int outstart, int outsize, resultList *r1, resultList *r2){ Accum acc; int n = 0; int outend = outstart + outsize; if (r2->num == 0){ job_result_printf(wi, "OUTOF %d", 0); return; } for (int i = 0; i < r1->num; ++i){ result_type r = r1->buf[i] & cat_mask; if (r >= maxcat) continue; result_type m = r2->mask[r]; if (m != 0){ n++; if (n <= outstart) continue; job_result_queue(wi, acc, r1->buf[i] + ((m - 1) << depth_shift), r2->tags == NULL ? goodImages->tags[r] : r2->tags[r]); if (n >= outend) break; } } job_result_flush(wi, acc); job_result_printf(wi, "OUTOF %d", n - outstart); }
static void findFQV_emit_item(WorkItem *wi, int outstart, int outsize, resultList *r1){ if (r1->num == 0){ job_result_printf(wi, "OUTOF %d", 0); return; } Accum acc; int n = 0; unsigned char k; int i, outend = outstart + outsize; result_type r, m; for (k = 1; k <= 4; ++k){ for (i = 0; i < r1->num; ++i){ r = r1->buf[i] & cat_mask; if (r >= maxcat) continue; m = goodImages->mask[r]; if (m != 0 && k == goodImages->tags[r]){ n++; if (n <= outstart) continue; job_result_queue(wi, acc, r1->buf[i] + ((m - 1) << depth_shift), goodImages->tags[r]); if (n >= outend) break; } } if (n >= outend) break; } job_result_flush(wi, acc); if (k == 5) job_result_printf(wi, "OUTOF %d", n - outstart); else if (((k - 1) * r1->num + i) > 0) job_result_printf(wi, "OUTOF %d", (outend * r1->num * 3) / ((k - 1) * r1->num + i)); }

static void compute_thread_fn(){
  for(;;){
    WorkItem *wi = nullptr;
    {
      std::unique_lock<std::mutex> lk(gq_mtx);
      gq_cv.wait(lk, []{ return !gqueue.empty(); });
      wi = gqueue.front(); gqueue.pop_front(); g_current = wi;
    }
    if (!wi) continue;
    int nr = 0; // number of result lists used
    // validate input
    if (wi->q.c1 < 0 || wi->q.c1 >= maxcat) { job_result_done(wi); goto done; }
    {
      int c2 = (wi->q.c2 >= 0 ? (int)wi->q.c2 : (int)wi->q.c1);
      if (c2 < 0 || c2 >= maxcat) { job_result_done(wi); goto done; }
      if (isFile((int)wi->q.c1) || (isFile(c2) && wi->q.a != "path")) { job_result_done(wi); goto done; }
    }

    // start
    wi->status = JS_PREPROCESS; job_result_start(wi);

    if (wi->q.a == std::string("path")){
      result[0]->clear(); wi->status = JS_STREAMING; tagCat_emit_item((tree_type)wi->q.c1, (tree_type)(wi->q.c2>=0?wi->q.c2:wi->q.c1), (int)wi->q.d1, result[0], wi);
    } else {
      result[0]->num = 0; result[1]->num = 0; int cid[2] = {(int)wi->q.c1, (int)(wi->q.c2>=0?wi->q.c2:wi->q.c1)}; int depth[2] = {(int)wi->q.d1, (int)wi->q.d2};
      nr = (wi->q.a == "list" || wi->q.a == "fqv") ? 1 : 2;
      for (int j = 0; j < nr; ++j){ result[j]->clear(); fetchFiles(cid[j], depth[j], result[j]); }
      wi->status = JS_COMPUTING;
      if (wi->q.a == "list") traverse_emit_item(wi, wi->q.o, wi->q.s, result[0]);
      else if (wi->q.a == "fqv") findFQV_emit_item(wi, wi->q.o, wi->q.s, result[0]);
      else if (wi->q.a == "not") notin_emit_item(wi, wi->q.o, wi->q.s, result[0], result[1]);
      else intersect_emit_item(wi, wi->q.o, wi->q.s, result[0], result[1]);
    }
    {
      time_t now = time(NULL); job_result_printf(wi, "DBAGE %.f", difftime(now, treetime));
      for (int j = 0; j < nr; ++j) result[j]->shrink();
    }
    wi->status = JS_DONE; job_result_done(wi);
done:
    {
      std::lock_guard<std::mutex> lk(gq_mtx); g_current = nullptr;
    }
  }
}

// --------------------- HTTP protocol callback ---------------------
static int callback_http(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len){
  (void)in; (void)len; auto *pss = (pss_http*)user;
  switch (reason){
    case LWS_CALLBACK_HTTP: {
      // path routing: status vs compute
      char uri[256]; if (lws_hdr_copy(wsi, uri, sizeof(uri), WSI_TOKEN_GET_URI) <= 0) uri[0] = 0;
      if (strcmp(uri, "/status") == 0){
        // build JSON
        double la[3] = {0,0,0}; getloadavg(la, 3);
        time_t now = time(NULL);
        int qlen; { std::lock_guard<std::mutex> lk(gq_mtx); qlen = (int)gqueue.size() + (g_current ? 1 : 0); }
        char body[256]; snprintf(body, sizeof(body), "{\"queue\":%d,\"relsize\":%d,\"dbage\":%.f,\"load\":[%f,%f,%f]}", qlen, maxcat, difftime(now, treetime), la[0], la[1], la[2]);
        uint8_t buf[LWS_PRE + 512]; uint8_t *p = &buf[LWS_PRE], *end = &buf[sizeof(buf) - 1];
        if (lws_add_http_common_headers(wsi, 200, "application/json; charset=utf-8", (lws_filepos_t)strlen(body), &p, end)) return 1;
        if (lws_finalize_write_http_header(wsi, &buf[LWS_PRE], &p, end)) return 1;
        lws_write(wsi, (unsigned char*)body, (unsigned int)strlen(body), LWS_WRITE_HTTP);
        return -1;
      }

      // compute request: stream text or JS
      Query q = parse_query(wsi);
      bool use_js = false; { std::string t = get_arg(wsi, "t"); use_js = (t == "js"); }
      pss->wsi = wsi; pss->js = use_js; pss->closed = false; pss->js_header_sent = false;
      const char *ctype = use_js ? "application/javascript; charset=utf-8" : "text/plain; charset=utf-8";
      uint8_t buf[LWS_PRE + 512]; uint8_t *p = &buf[LWS_PRE], *end = &buf[sizeof(buf) - 1];
      if (lws_add_http_common_headers(wsi, 200, ctype, (lws_filepos_t)-1, &p, end)) return 1;
      if (lws_finalize_write_http_header(wsi, &buf[LWS_PRE], &p, end)) return 1;

      // enqueue work
      auto *wi = new WorkItem; wi->q = q; wi->ct = use_js ? CT_HTTP_JS : CT_HTTP; wi->ph = pss; pss->wi = wi;
      {
        std::lock_guard<std::mutex> lk(gq_mtx); gqueue.push_back(wi);
      }
      gq_cv.notify_one();
      // for JS, send opening callback
      if (use_js){ std::lock_guard<std::mutex> lk(pss->qmtx); pss->q.push_back("fastcciCallback( [\n"); pss->js_header_sent = true; }
      lws_callback_on_writable(wsi);
      return 0; }
    case LWS_CALLBACK_HTTP_WRITEABLE: {
      // drain queued lines
      for(;;){
        std::string line; {
          std::lock_guard<std::mutex> lk(pss->qmtx); if (pss->q.empty()) break; line = std::move(pss->q.front()); pss->q.pop_front(); }
        if (pss->js){
          // for JS, if line is DONE -> close array+cb and finish
          if (line.rfind("DONE", 0) == 0){
            std::string closing = " 'DONE'] );\n";
            int n = lws_write(wsi, (unsigned char*)closing.data(), (unsigned int)closing.size(), LWS_WRITE_HTTP);
            if (n < 0) return -1; lws_write(wsi, (unsigned char*)"", 0, LWS_WRITE_HTTP_FINAL); return -1;
          }
          // else wrap as 'line',
          // strip trailing \n
          if (!line.empty() && line.back()=='\n') line.pop_back();
          std::string wrapped = " '" + line + "',";
          int n = lws_write(wsi, (unsigned char*)wrapped.data(), (unsigned int)wrapped.size(), LWS_WRITE_HTTP);
          if (n < 0) return -1;
        } else {
          int n = lws_write(wsi, (unsigned char*)line.data(), (unsigned int)line.size(), LWS_WRITE_HTTP);
          if (n < 0) return -1;
          if (line.rfind("DONE", 0) == 0){ lws_write(wsi, (unsigned char*)"", 0, LWS_WRITE_HTTP_FINAL); return -1; }
        }
      }
      return 0; }
    case LWS_CALLBACK_CLOSED_HTTP:
      pss->closed = true; pss->wsi = nullptr; if (pss->wi) pss->wi->ph = nullptr; return 0;
    default: return 0;
  }
}

// --------------------- WS protocol callback ---------------------
static int callback_ws(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len){
  (void)in; (void)len; auto *pss = (pss_ws*)user;
  switch (reason){
    case LWS_CALLBACK_ESTABLISHED: {
      pss->ring = lws_ring_create(sizeof(msg), 512, destroy_msg); pss->tail = 0; pss->wsi = wsi;
      Query q = parse_query(wsi);
      // enqueue work
      auto *wi = new WorkItem; wi->q = q; wi->ct = CT_WS; wi->pw = pss; pss->wi = wi;
      {
        std::lock_guard<std::mutex> lk(gq_mtx);
        // compute position and inform client
        int pos = (int)gqueue.size() + (g_current ? 1 : 0);
        char msgbuf[64]; snprintf(msgbuf, sizeof(msgbuf), "QUEUED %d\n", pos);
        emit_to_ws(pss, std::string(msgbuf));
        gqueue.push_back(wi);
      }
      gq_cv.notify_one();
      return 0; }
    case LWS_CALLBACK_SERVER_WRITEABLE: {
      if (!pss->ring) return 0; const msg *pm = (const msg*)lws_ring_get_element(pss->ring, &pss->tail); if (!pm) return 0;
      std::vector<unsigned char> buf(LWS_PRE + pm->len); memcpy(&buf[LWS_PRE], pm->data, pm->len);
      int n = lws_write(wsi, &buf[LWS_PRE], (unsigned int)pm->len, LWS_WRITE_TEXT); if (n < 0) return -1;
      bool is_done = (pm->len >= 4 && !memcmp(pm->data, "DONE", 4));
      lws_ring_consume(pss->ring, &pss->tail, NULL, 1); lws_ring_update_oldest_tail(pss->ring, pss->tail);
      if (is_done) return -1; if (lws_ring_get_element(pss->ring, &pss->tail)) lws_callback_on_writable(wsi); return 0; }
    case LWS_CALLBACK_CLOSED:
      pss->closed = true; pss->wsi = nullptr; if (pss->ring) { lws_ring_destroy(pss->ring); pss->ring = nullptr; } if (pss->wi) pss->wi->pw = nullptr; return 0;
    default: return 0;
  }
}

int main(int argc, char **argv) {
  // initialize DB
  if (argc != 3) { fprintf(stderr, "%s PORT DATADIR\n", argv[0]); return 1; }
  rbInit(rb);
  const int buflen = 1000; char fname[buflen];
  snprintf(fname, buflen, "%s/fastcci.cat", argv[2]); unsigned int cat_file_len = readFile(fname, cat); maxcat = cat_file_len / sizeof(tree_type);
  result[0] = new resultList(1024 * 1024); result[1] = new resultList(1024 * 1024); goodImages = new resultList(512);
  if ((parent = (tree_type *)malloc(maxcat * sizeof *parent)) == NULL){ perror("parent"); exit(1); }
  snprintf(fname, buflen, "%s/fastcci.tree", argv[2]); unsigned int tree_file_len = readFile(fname, tree);
  struct stat statbuf; if (stat(fname, &statbuf) == -1) { perror(fname); exit(1);} treetime = statbuf.st_mtime;
  // precompute goodImages
  int goodCats[][3] = {{3943817,0,1},{5799448,1,1},{91039287,0,2},{3618826,0,3},{4143367,0,4}};
  goodImages->clear(); goodImages->addTags(); result_type r;
  for (int i = 5; i > 0; --i){ result[0]->clear(); result[0]->num = 0; fetchFiles(goodCats[i-1][0], goodCats[i-1][1], result[0]); for (int j = 0; j < result[0]->num; j++){ r = result[0]->buf[j] & cat_mask; if (r < maxcat){ goodImages->mask[r] = result[0]->mask[r]; goodImages->tags[r] = goodCats[i-1][2]; } } }
  goodImages->num = -1;

  // Start compute worker and notifier
  pthread_t compute_thread; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
  if (pthread_create(&compute_thread, &attr, [](void*)->void*{ compute_thread_fn(); return nullptr; }, nullptr)) return 1;
  pthread_t notify_thread; if (pthread_create(&notify_thread, &attr, [](void*)->void*{
    timespec ts; ts.tv_sec = 0; ts.tv_nsec = 200 * 1000 * 1000; // 200ms
    for(;;){
      nanosleep(&ts, nullptr);
      // snapshot queue
      std::vector<WorkItem*> snapshot; WorkItem *cur;
      {
        std::lock_guard<std::mutex> lk(gq_mtx); snapshot.assign(gqueue.begin(), gqueue.end()); cur = g_current;
      }
      // send WAITING for queued WS items
      for (size_t i = 0; i < snapshot.size(); ++i){ WorkItem *wi = snapshot[i]; if (wi && wi->ct == CT_WS && wi->pw){ char msgbuf[64]; snprintf(msgbuf, sizeof(msgbuf), "WAITING %zu\n", snapshot.size() - i); emit_to_ws(wi->pw, std::string(msgbuf)); } }
      // send WORKING for current
      if (cur && cur->ct == CT_WS && cur->pw){ if (cur->status == JS_PREPROCESS || cur->status == JS_COMPUTING){ char msgbuf[64]; snprintf(msgbuf, sizeof(msgbuf), "WORKING %d %d\n", result[0]->num, result[1]->num); emit_to_ws(cur->pw, std::string(msgbuf)); } }
    }
    return nullptr; }, nullptr)) return 1;

  // LWS context
  lws_set_log_level(LLL_USER | LLL_NOTICE, nullptr);
  struct lws_context_creation_info info; memset(&info, 0, sizeof(info)); info.port = atoi(argv[1]);
  static const struct lws_protocols protocols[] = {
    { "http", callback_http, sizeof(pss_http), 0 },
    { "fastcci-ws", callback_ws, sizeof(pss_ws), 0 },
    { nullptr, nullptr, 0, 0 }
  }; info.protocols = protocols;
  struct lws_context *context = lws_create_context(&info); if (!context) return 1;
  while (lws_service(context, 0) >= 0) { /* loop */ }
  lws_context_destroy(context);
  munmap(cat, cat_file_len); munmap(tree, tree_file_len);
  return 0;
}
