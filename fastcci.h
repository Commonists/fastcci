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
enum wiConn { WC_XHR, WC_SOCKET, WC_JS, WC_JS_CONT };

// work item type
enum wiType { WT_INTERSECT, WT_TRAVERSE, WT_NOTIN, WT_PATH, WT_FQV };

// work item status type
enum wiStatus { WS_WAITING, WS_PREPROCESS, WS_COMPUTING, WS_STREAMING, WS_DONE };


// breadth first search ringbuffer (TODO: OOP)
struct ringBuffer {
  int size, mask, a, b;
  result_type *buf;
};


void rbInit(ringBuffer &rb) {
  rb.size = 1024;
  rb.mask = rb.size-1;
  if ((rb.buf = (result_type*)malloc(rb.size * sizeof(result_type)) ) == NULL) {
    perror("rbInit()");
    exit(1);
  }
}
void rbClear(ringBuffer &rb) {
  rb.a = 0;
  rb.b = 0;
}
inline bool rbEmpty(ringBuffer &rb) {
  return rb.a == rb.b;
}
void rbGrow(ringBuffer &rb) {
  if ((rb.buf = (result_type*)realloc(rb.buf, 2 * rb.size * sizeof *(rb.buf)) ) == NULL) {
    perror("rbGrow()");
    exit(1);
  }

  fprintf(stderr,"Ring buffer grow: a=%d b=%d size=%d\n", rb.a, rb.b, rb.size );
  memcpy( &(rb.buf[rb.size]), rb.buf, rb.size * sizeof *(rb.buf) );
  rb.size *= 2;
  rb.mask = rb.size-1; 
}
inline void rbPush(ringBuffer &rb, result_type r) {
  if (rb.b-rb.a >= rb.size) rbGrow(rb);
  rb.buf[(rb.b++) & rb.mask] = r;
}
inline result_type rbPop(ringBuffer &rb) {
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
  fprintf(stderr, "Loading %s ...\n", fname);
  FILE *in = fopen(fname,"rb");
  if (in==NULL) {
    perror(fname);
    exit(1);
  }

  // determine file size
  fseek(in, 0L, SEEK_END);
  int sz = ftell(in);
  fseek(in, 0L, SEEK_SET);

  // allocate memory for entire file
  buf = (tree_type*)malloc(sz);
  if (buf==NULL) {
    perror("readFile()");
    exit(1);
  }

  // read file into memory
  int sz2 = fread(buf, 1, sz, in);
  return sz;
}

int compare (const void * a, const void * b) {
  return ( *(tree_type*)b - *(tree_type*)a );
}


