#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#if !defined(__APPLE__)
#include <malloc.h>
#endif
#include <string.h>
#include <pthread.h>
#include <sys/types.h>

typedef int32_t tree_type; 
typedef int64_t result_type;
const int depth_shift = 32;
const result_type depth_mask = result_type(0x7FFFFFFF) << depth_shift;
const result_type cat_mask   = 0x7FFFFFFF;

#include <onion/onion.h>
#include <onion/handler.h>
#include <onion/response.h>
#include <onion/websocket.h>

// work item type
enum wiConn { WC_NULL, WC_XHR, WC_SOCKET, WC_JS, WC_JS_CONT };

// work item type
enum wiType { WT_INTERSECT, WT_TRAVERSE, WT_NOTIN, WT_PATH };

// work item status type
enum wiStatus { WS_WAITING, WS_PREPROCESS, WS_COMPUTING, WS_STREAMING, WS_DONE };


// breadth first search ringbuffer
struct ringBuffer {
  int size, mask, a, b;
  result_type *buf;
};


int rbInit(ringBuffer &rb) {
  rb.size = 1024;
  rb.mask = rb.size-1;
  rb.buf = (result_type*)malloc(rb.size * sizeof(result_type));
}
int rbClear(ringBuffer &rb) {
  rb.a = 0;
  rb.b = 0;
}
inline bool rbEmpty(ringBuffer &rb) {
  return rb.a == rb.b;
}
int rbGrow(ringBuffer &rb) {
  rb.buf = (result_type*)realloc(rb.buf, 2 * rb.size * sizeof *(rb.buf) );
  fprintf(stderr,"Ring buffer grow: a=%d b=%d size=%d\n", rb.a, rb.b, rb.size );
  memcpy( &(rb.buf[rb.size]), rb.buf, rb.size * sizeof *(rb.buf) );
  rb.size *= 2;
  rb.mask = rb.size-1; 
}
inline void rbPush(ringBuffer &rb, result_type r) {
  if (rb.b-rb.a >= rb.size) rbGrow(rb);
  //fprintf(stderr,"Ring buffer push %d,%d   a=%d b=%d size=%d\n", r&cat_mask, (r&depth_mask) >> depth_shift, rb.a, rb.b, rb.size );
  rb.buf[(rb.b++) & rb.mask] = r;
}
inline result_type rbPop(ringBuffer &rb) {
  //fprintf(stderr,"Ring buffer pop %d,%d   a=%d b=%d size=%d\n", rb.buf[rb.a & rb.mask]&cat_mask,(rb.buf[rb.a & rb.mask]&depth_mask) >> depth_shift, rb.a, rb.b, rb.size );
  return rb.buf[(rb.a++) & rb.mask];
}


// work item queue 
struct workItem {
  // thread data
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  // response
  onion_response *res;
  onion_websocket *ws;
  // query parameters
  int c1, c2; // categories
  int d1, d2; // depths
  // offset and size
  int o,s;
  // conenction type
  wiConn connection;
  // job type
  wiType type;
  // status
  wiStatus status;
  int t0; // queuing timestamp
};

int readFile(const char *fname, tree_type* &buf) {
  FILE *in = fopen(fname,"rb");
  fseek(in, 0L, SEEK_END);
  int sz = ftell(in);
  fseek(in, 0L, SEEK_SET);
  buf = (tree_type*)malloc(sz);
  int sz2 = fread(buf, 1, sz, in);
  return sz;
}

int compare (const void * a, const void * b) {
  return ( *(tree_type*)a - *(tree_type*)b );
}


