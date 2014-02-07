#include <time.h>
#include "fastcci.h"
#include <sys/stat.h>

// thread management objects
pthread_mutex_t handlerMutex;
pthread_mutex_t mutex;
pthread_cond_t condition;

// category data and traversal information
const int maxdepth=500;
int maxcat; 
tree_type *cat, *tree, *parent; 

// modification time of the tree database file
time_t treetime;

// new result data structure
struct resultList {
  int max, num;
  result_type *buf;
  unsigned char *mask, *tags;
  resultList(int initialSize = 1024*1024) : max(initialSize), num(0), tags(NULL) {
    buf = (result_type*)malloc(max * sizeof *buf);
    mask = (unsigned char*)malloc(maxcat * sizeof *mask);

    if (buf==NULL || mask==NULL) {
      perror("resultList()");
      exit(1);
    }
  }

  // pointer to element after the last result
  result_type* tail() const { return &(buf[num]); }  

  // grow buffer to hold at least len more items
  void grow(int len) {
    if (num+len > max) {
      while (num+len > max) max *= 2;
      if ((buf = (result_type*)realloc(buf, max * sizeof *buf)) == NULL) {
        perror("resultList->grow()");
        exit(1);
      }
    }
  }

  // attempt to shrink large buffers to half their size
  void shrink() {
    if (num < max/2 && max>(1024*1024)) {
      max /= 2;
      if ((buf = (result_type*)realloc(buf, max * sizeof *buf)) == NULL) {
        perror("resultList->shrink()");
        exit(1);
      }
    }
  }

  // clear mask
  void clear() {
    memset(mask,0,maxcat * sizeof *mask);
  }

  // tags list for special union groups (to identify FP/QI/VI for example)
  void addTags() {
    if ((tags = (unsigned char*)calloc(maxcat, sizeof(*tags))) == NULL) {
      perror("resultList->addTags()");
      exit(1);
    }
  }

  // sort result list (unused for output)
  void sort() {
    qsort(buf, num, sizeof *buf, compare);
  }
};

resultList *result[2], *goodImages;

// breadth first search ringbuffer
struct ringBuffer rb;

// work item queue 
int aItem = 0, bItem = 0;
const int maxItem = 1000;
struct workItem queue[maxItem];

// check if an ID is a valid category
inline bool isCategory(int i) {
  return (i>=0 && i<maxcat && cat[i]>=0);
}

// check if an ID is a valid file
inline bool isFile(int i) {
  return (i>=0 && i<maxcat && cat[i]<0);
}

// buffering of up to 50 search results (the amount we can safely API query)
const int resmaxqueue = 50, resmaxbuf = 64*resmaxqueue;
char rescombuf[resmaxbuf];
int resnumqueue=0, residx=0;
ssize_t resultPrintf(int i, const char *fmt, ...) {
  int ret;
  char buf[4096]; // big enough for resmaxbuf!
  va_list myargs;

  va_start(myargs, fmt);
  ret = vsnprintf(buf, 4096, fmt, myargs);
  va_end(myargs);

  onion_response *res = queue[i].res;
  onion_websocket *ws = queue[i].ws;

  // TODO: use onion_*_write here?
  if (res) {
    // regular text response
    if (queue[i].connection==WC_XHR) 
      return onion_response_printf(res, "%s\n", buf);
    // wrap response in a callback call (first data item)
    else if (queue[i].connection==WC_JS) 
      return onion_response_printf(res, " '%s',", buf);
  }
  if (ws)  return onion_websocket_printf(ws, "%s", buf);
  return 0;
}
void resultDone(int i) {
  queue[i].status = WS_DONE;
  onion_response *res = queue[i].res;
  onion_websocket *ws = queue[i].ws;

  // TODO: use onion_*_write here?
  if (res) {
    // regular text response
    if (queue[i].connection==WC_XHR) 
      onion_response_printf(res, "DONE\n");
    else 
      onion_response_printf(res, " 'DONE'] );\n");
  }
  if (ws)  onion_websocket_printf(ws, "DONE");
}
void resultStart(int i) {
  onion_response *res = queue[i].res;
  onion_websocket *ws = queue[i].ws;

  if (res && queue[i].connection==WC_JS) onion_response_printf(res, "fastcciCallback( [");
  if (queue[i].ws ) onion_websocket_printf(queue[i].ws, "COMPUTE_START");  
}
void resultFlush(int i) {
  // nothing to flush
  if (residx==0) return;

  // 
  onion_response *res = queue[i].res;
  onion_websocket *ws = queue[i].ws;

  // zero terminate buffer
  rescombuf[residx-1] = 0;

  // send buffer and reset inices
  resultPrintf(i, "RESULT %s", rescombuf);
  resnumqueue=0; residx=0;
}
void resultQueue(int i, result_type item, unsigned char tag) {
  // TODO check for truncation (but what then?!)
  residx += snprintf(&(rescombuf[residx]), resmaxbuf-residx, "%d,%d,%d|", int(item & cat_mask), int((item & depth_mask)>>depth_shift), tag);

  // queued enough values?
  if (++resnumqueue == resmaxqueue) resultFlush(i);
}


//
// Fetch all files in and below category 'id'
// in a breadth first search up to depth 'depth'
// if 'depth' id negative treat it as infinity
//
void fetchFiles(tree_type id, int depth, resultList *r1) {
  // clear ring buffer
  rbClear(rb);

  // push root node (depth 0)
  rbPush(rb,id);

  result_type r, d,e, i;
  unsigned char f;
  int c, len;
  while (!rbEmpty(rb)) {
    r = rbPop(rb);
    d = (r & depth_mask) >> depth_shift;
    i = r & cat_mask;
    
    // tag current category as visited
    r1->mask[i]=1;

    int c = cat[i], cend = tree[c], cfile = tree[c+1];
    c += 2;

    // push all subcat to queue
    if (d<depth || depth<0) {
      e = (d+1)<<depth_shift;
      while (c<cend) {
        // push unvisited categories (that are not empty, cat[id]==0) into the queue
        if (r1->mask[tree[c]]==0 && cat[tree[c]]>0) {
          rbPush(rb, tree[c] | e);
        }
        c++;
      }
    }

    // copy and add the depth on top
    int len = cfile-c;
    r1->grow(len);
    result_type *dst = r1->tail(), *old = dst;
    tree_type   *src = &(tree[c]);
    f = d<254 ? (d+1) : 255;
    d = d<<depth_shift;
    while (len--) {
      r =  (*src++); 
      if (r1->mask[r & cat_mask]==0) {
        *dst++ = (r | d);
        r1->mask[r] = f;
      }
    }
    r1->num += dst-old;
  }
}

//
// iteratively do a breadth first path search
//
result_type history[maxdepth]; 
void tagCat(tree_type sid, int qi, int maxDepth, resultList *r1) {
  // clear ring buffer
  rbClear(rb);

  bool foundPath = false, c2isFile = (cat[queue[qi].c2]<0);
  int depth = -1;
  result_type id  = sid, 
              did = queue[qi].c2;

  // push root node (depth 0)
  rbPush(rb,sid);

  result_type r, d,e, ld = -1;
  int c, len;
  while (!rbEmpty(rb) && !foundPath) {
    r  = rbPop(rb);
    d  = (r & depth_mask) >> depth_shift;
    id = r & cat_mask;
    
    // next layer?
    if (d!=ld) {
      depth++;
      ld = d;
    }

    // head category header
    int c = cat[id], cend = tree[c], cend2 = tree[c+1];
    c += 2;

    // push all subcat to queue
    if (depth<maxDepth || maxDepth<0) {
      e = (d+1)<<depth_shift;

      while (c<cend) {
        // inspect if we are pushing the target cat to the queue
        if (tree[c]==did) {
          parent[did] = id;
          id = did;
          foundPath = true;
          break;
        }

        // push unvisited categories into the queue
        if (r1->mask[tree[c]]==0) {
          parent[tree[c]] = id;
          r1->mask[tree[c]]   = 1;
          rbPush(rb, tree[c] | e);
        }
        c++;
      }
    }

    // check if a file in the category is a match
    if (c2isFile) {
      for (c=cend; c<cend2; ++c) {
        if (tree[c]==did) {
          foundPath=true;
          break;
        }
      }
    }
  }

  // found the target category
  if (foundPath) {
    // TODO backtrack through the parent category buffer
    int i = 0;
    while(true) {
      history[i++] = id;
      if (id==sid) break;
      id = parent[id];
    } 
    int len = i;
    // output in reverse to get the forward chain
    while (i--) resultQueue(qi, history[i] + (result_type(len-i)<<depth_shift), 0);
    resultFlush(qi);
  } else {
    resultPrintf(qi, "NOPATH"); 
  }
}

//
// all images in result
//
void traverse(int qi, resultList *r1) {
  int outstart = queue[qi].o;
  int outend   = outstart + queue[qi].s;
  onion_response *res = queue[qi].res;
  onion_websocket *ws = queue[qi].ws;

  // output selected subset
  queue[qi].status = WS_STREAMING;
  if (outend>r1->num) outend=r1->num;
  result_type r;
  for (int i=outstart; i<outend; ++i) {
    r = r1->buf[i] & cat_mask;
    resultQueue(qi, r1->buf[i], r1->tags==NULL ? goodImages->tags[r] : r1->tags[r]);
  }
  resultFlush(qi);

  // send the (exact) size of the complete result set
  resultPrintf(qi, "OUTOF %d", r1->num); 
}

//
// all images in result that are not flagged in the mask
//
void notin(int qi, resultList *r1, resultList *r2) {
  int n = 0; // number of current output item

  int outstart = queue[qi].o;
  int outend   = outstart + queue[qi].s;
  onion_response *res = queue[qi].res;
  onion_websocket *ws = queue[qi].ws;

  // No sorting, show least depth first, use mask for NOT test
  fprintf(stderr,"using mask strategy.\n");

  // perform subtraction
  queue[qi].status = WS_STREAMING;
  result_type r;
  int i;
  for (i=0; i<r1->num; ++i) {
    r = r1->buf[i] & cat_mask;

    if (r2->mask[r]==0) {
      n++;
      // are we still below the offset?
      if (n<=outstart) continue;
      // output file      
      resultQueue(qi, r1->buf[i], r1->tags==NULL ? goodImages->tags[r] : r1->tags[r]);
      // are we at the end of the output window?
      if (n>=outend) break;
    }
  }

  resultFlush(qi);

  // did we make it all the way to the end of the result set?
  if (i==r1->num) 
    resultPrintf(qi, "OUTOF %d", n-outstart);
  // otherwise make a crude guess based on the progress within result r1
  else if(i>0)
    resultPrintf(qi, "OUTOF %d", (outend*r1->num)/i);
}

//
// all images in both c1 and c2 output is sorted by depth in c1
//
void intersect(int qi, resultList *r1, resultList *r2) {
  int n = 0; // number of current output item

  int outstart = queue[qi].o;
  int outend   = outstart + queue[qi].s;
  onion_response *res = queue[qi].res;
  onion_websocket *ws = queue[qi].ws;

  // was one of the results empty?
  if (r2->num==0) {
    resultPrintf(qi, "OUTOF %d", 0); 
    return;
  }

  // perform intersection
  queue[qi].status = WS_STREAMING;
  result_type r, m;

  int i;
  for (i=0; i<r1->num; ++i) {
    r = r1->buf[i] & cat_mask;
    m = r2->mask[r];
    if (m!=0) {
      n++;
      // are we still below the offset?
      if (n<=outstart) continue;
      // output file      
      resultQueue(qi, r1->buf[i] + ((m-1)<<depth_shift), r2->tags==NULL ? goodImages->tags[r] : r2->tags[r] );
      // are we at the end of the output window?
      if (n>=outend) break;
    }
  }

  resultFlush(qi);

  // did we make it all the way to the end of the result set?
  if (i==r1->num) 
    resultPrintf(qi, "OUTOF %d", n-outstart);
  // otherwise make a crude guess based on the progress within result r1
  else if(i>0)
    resultPrintf(qi, "OUTOF %d", (outend*r1->num)/i);
}

//
// all FPs, QIs, VIs in c1 in that order
//
void findFQV(int qi, resultList *r1) {
  int n = 0; // number of current output item

  int outstart = queue[qi].o;
  int outend   = outstart + queue[qi].s;
  onion_response *res = queue[qi].res;
  onion_websocket *ws = queue[qi].ws;

  // was one of the results empty?
  if (r1->num==0) {
    resultPrintf(qi, "OUTOF %d", 0); 
    return;
  }

  // perform intersection
  queue[qi].status = WS_STREAMING;
  result_type r, m;

  // loop over tags
  unsigned char k;
  int i;
  for (k=1; k<=3; ++k ) {
    // loop over c1 images
    for (i=0; i<r1->num; ++i) {
      r = r1->buf[i] & cat_mask;
      m = goodImages->mask[r];
      if (m!=0 && k==goodImages->tags[r]) {
        n++;
        // are we still below the offset?
        if (n<=outstart) continue;
        // output file      
        resultQueue(qi, r1->buf[i] + ((m-1)<<depth_shift), goodImages->tags[r] );
        // are we at the end of the output window?
        if (n>=outend) break;
      }
    }
    if (n>=outend) break;
  }

  resultFlush(qi);

  // did we make it all the way to the end of the result set?
  if (k==4) 
    resultPrintf(qi, "OUTOF %d", n-outstart);
  // otherwise make a crude guess
  else if (((k-1)*r1->num + i)>0)
    resultPrintf(qi, "OUTOF %d", (outend * r1->num*3) / ((k-1)*r1->num + i));
}


onion_connection_status handleStatus(void *d, onion_request *req, onion_response *res)
{
  pthread_mutex_lock(&mutex);
  onion_response_printf(res, "%d requests in the queue.\n", bItem-aItem);
  
  // list all active queue items 
  //for (int i=aItem; i<bItem; ++i)
  //  onion_response_printf(res, "");

  pthread_mutex_unlock(&mutex);
  return OCS_CLOSE_CONNECTION;
}

onion_connection_status handleRequest(void *d, onion_request *req, onion_response *res)
{
  // parse parameters
  const char* c1 = onion_request_get_query(req, "c1");
  const char* c2 = onion_request_get_query(req, "c2");

  if (c1==NULL) {
    // must supply c1 parameter!
    fprintf(stderr,"No c1 parameter.\n");
    return OCS_INTERNAL_ERROR;
  }

  // still room on the queue?
  pthread_mutex_lock(&mutex);
  if( bItem-aItem+1 >= maxItem ) {
    // too many requests. reject
    fprintf(stderr,"Queue full.\n");
    pthread_mutex_unlock(&mutex);
    return OCS_INTERNAL_ERROR;
  }
  // new queue item
  int i = bItem % maxItem;
  pthread_mutex_unlock(&mutex);

  queue[i].c1 = atoi(c1);
  queue[i].c2 = c2 ? atoi(c2) : queue[i].c1;

  const char* d1 = onion_request_get_query(req, "d1");
  const char* d2 = onion_request_get_query(req, "d2");

  const char* oparam = onion_request_get_query(req, "o");
  const char* sparam = onion_request_get_query(req, "s");

  queue[i].d1 = d1 ? atoi(d1) : -1;
  queue[i].d2 = d2 ? atoi(d2) : -1;

  queue[i].o = oparam ? atoi(oparam) : 0;
  queue[i].s = sparam ? atoi(sparam) : 100;

  queue[i].status = WS_WAITING;

  const char* aparam = onion_request_get_query(req, "a");

  queue[i].type = WT_INTERSECT;
  if (aparam != NULL) {
    if (strcmp(aparam,"and")==0)
      queue[i].type = WT_INTERSECT;
    else if (strcmp(aparam,"not")==0)
      queue[i].type = WT_NOTIN;
    else if (strcmp(aparam,"fqv")==0)
      queue[i].type = WT_FQV;
    else if (strcmp(aparam,"list")==0)
      queue[i].type = WT_TRAVERSE;
    else if (strcmp(aparam,"path")==0) {
      queue[i].type = WT_PATH;
      if (queue[i].c1==queue[i].c2) return OCS_INTERNAL_ERROR;
    }
    else
      return OCS_INTERNAL_ERROR;
  }

  // check if invalid ids were specified
  if (queue[i].c1>=maxcat || queue[i].c2>=maxcat || queue[i].c1<0 || queue[i].c2<0 ) return OCS_INTERNAL_ERROR;

  // check if both c params are categories unless it is a path request
  if (isFile(queue[i].c1) || (isFile(queue[i].c2) && queue[i].type!=WT_PATH) ) return OCS_INTERNAL_ERROR;

  // log request
  if (aparam==NULL) aparam="and";
  fprintf(stderr, "Request [%ld %d]: a=%s c1=%d(%d) c2=%d(%d)\n", time(NULL), bItem-aItem, aparam, queue[i].c1, queue[i].d1, queue[i].c2, queue[i].d2);

  // attempt to open a websocket connection
  onion_websocket *ws = onion_websocket_new(req, res);
  if (!ws) {
    //
    // HTTP connection (fallback)
    //

    // send keep-alive response
    onion_response_set_header(res, "Access-Control-Allow-Origin", "*");

    // shuld we send a javascript callback for older browsers?
    const char* tparam = onion_request_get_query(req, "t");
    if (tparam!=NULL && strcmp(tparam,"js")==0) {
      queue[i].connection = WC_JS;
      onion_response_set_header(res, "Content-Type","application/javascript; charset=utf8");
    } else {
      queue[i].connection = WC_XHR;
      onion_response_set_header(res, "Content-Type","text/plain; charset=utf8");
    }

    queue[i].res = res;
    queue[i].ws  = NULL;

    // append to the queue and signal worker thread
    pthread_mutex_lock(&mutex);
    bItem++;
    pthread_cond_signal(&condition);
    pthread_mutex_unlock(&mutex);

    // wait for signal from worker thread 
    wiStatus status;
    do {
      pthread_mutex_lock(&(queue[i].mutex));
      pthread_cond_wait(&(queue[i].cond), &(queue[i].mutex));
      status = queue[i].status;
      pthread_mutex_unlock(&(queue[i].mutex));
    } while (status != WS_DONE);

  } else {
    //
    // Websocket connection
    //

    queue[i].connection = WC_SOCKET;
    queue[i].ws  = ws;
    queue[i].res = NULL;

    onion_websocket_printf(ws, "QUEUED %d", i-aItem);  

    // append to the queue and signal worker thread
    pthread_mutex_lock(&mutex);
    bItem++;
    pthread_cond_signal(&condition);
    pthread_mutex_unlock(&mutex);

    // wait for signal from worker thread (have a third thread periodically signal, only print result when the calculation is done, otherwise print status)
    wiStatus status;
    do {
      pthread_mutex_lock(&(queue[i].mutex));
      pthread_cond_wait(&(queue[i].cond), &(queue[i].mutex));
      status = queue[i].status;
      pthread_mutex_unlock(&(queue[i].mutex));

      fprintf(stderr,"notify status %d\n", status);
      switch (status) {
        case WS_WAITING :
          // send number of jobs ahead of this one in queue
          onion_websocket_printf(ws, "WAITING %d", i-aItem);  
          break;
        case WS_PREPROCESS :
        case WS_COMPUTING :
          // send intermediate result sizes
          onion_websocket_printf(ws, "WORKING %d %d", result[0]->num, result[1]->num);  
          break;
      }
      // don't do anything if status is WS_STREAMING, the compute task is sending data
    } while (status != WS_DONE);
  }

  fprintf(stderr,"End of handle connection.\n");
  return OCS_CLOSE_CONNECTION;
}

void *notifyThread( void *d ) {
  timespec updateInterval, t2; 
  updateInterval.tv_sec = 0;
  updateInterval.tv_nsec = 1000*1000*200; // 200ms

  while(1) {
    nanosleep(&updateInterval, &t2);
    
    pthread_mutex_lock(&handlerMutex);
    pthread_mutex_lock(&mutex);
    
    // loop over all active queue items (actually lock &mutex as well!)
    for (int i=aItem; i<bItem; ++i)
      if (queue[i].connection == WC_SOCKET)
        pthread_cond_signal(&(queue[i].cond));

    pthread_mutex_unlock(&mutex);
    pthread_mutex_unlock(&handlerMutex);
  }
}
void *computeThread( void *d ) {
  while(1) {
    // wait for pthread condition
    pthread_mutex_lock(&mutex);
    while (aItem == bItem) pthread_cond_wait(&condition, &mutex);
    pthread_mutex_unlock(&mutex);

    // process queue
    while (bItem>aItem) {
      // fetch next item
      int i = aItem % maxItem;

      // signal start of compute
      resultStart(i);

      int nr;
      if (queue[i].type==WT_PATH) {
        // path finding
        result[0]->clear();
        tagCat(queue[i].c1, i, queue[i].d1, result[0]);
      } else {
        // boolean operations (AND, LIST, NOTIN)
        result[0]->num = 0;
        result[1]->num = 0;
        queue[i].status = WS_PREPROCESS;

        // generate intermediate results
        int cid[2]   = {queue[i].c1, queue[i].c2};
        int depth[2] = {queue[i].d1, queue[i].d2};
        // number of result lists needed
        nr = (queue[i].type==WT_TRAVERSE || queue[i].type==WT_FQV) ? 1 : 2;
        for (int j=0; j<nr; ++j) {
          // clear visitation mask
          result[j]->clear();
          
          // fetch files through deep traversal
          fetchFiles(cid[j], depth[j], result[j]);
          fprintf(stderr,"fnum(%d) %d\n", cid[j], result[j]->num);
        }

        // compute result
        queue[i].status = WS_COMPUTING;

        switch (queue[i].type) {
          case WT_TRAVERSE :
            traverse(i, result[0]);
            break;
          case WT_FQV :
            findFQV(i, result[0]);
            break;

          case WT_NOTIN :
            notin(i, result[0], result[1]);
            break;
          case WT_INTERSECT :
            intersect(i, result[0], result[1]);
            break;
        }
      }

      // report database age
      time_t now = time(NULL);
      resultPrintf(i, "DBAGE %.f", difftime(now,treetime)); 

      // done with this request
      resultDone(i);

      // try to shrink result buffers uses in this request
      for (int j=0; j<nr; ++j) result[0]->shrink();

      // pop item off queue
      pthread_mutex_lock(&mutex);
      aItem++;
      pthread_mutex_unlock(&mutex);

      // wake up thread to finish connection
      pthread_mutex_lock(&handlerMutex);
      pthread_cond_signal(&(queue[i].cond));
      pthread_mutex_unlock(&handlerMutex);
    }
  }
}

int main(int argc, char *argv[]) {

  if (argc != 3) {
    printf("%s PORT DATADIR\n", argv[0]);
    return 1;
  }

  // ring buffer for breadth first
  rbInit(rb);

  const int buflen = 1000;
  char fname[buflen];

  snprintf(fname, buflen, "%s/fastcci.cat", argv[2]);
  maxcat = readFile(fname, cat);
  maxcat /= sizeof(tree_type);

  // result structures (including visitation mask buffer)
  result[0] = new resultList(1024*1024);
  result[1] = new resultList(1024*1024);
  goodImages = new resultList(512);

  // parent category buffer for shortest path finding
  if ((parent = (tree_type*)malloc(maxcat * sizeof *parent)) == NULL) {
    perror("parent");
    exit(1);
  }

  // read tree file
  snprintf(fname, buflen, "%s/fastcci.tree", argv[2]);
  readFile(fname, tree);

  // get modification time of tree file
  struct stat statbuf;
  if (stat(fname, &statbuf) == -1) {
    perror(fname);
    exit(1);
  }
  treetime = statbuf.st_mtime;

  // thread properties
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_JOINABLE );

  // setup compute thread
  pthread_t compute_thread;
  if (pthread_create(&compute_thread, &attr, computeThread, NULL)) return 1;

  // setup compute thread
  pthread_t notify_thread;
  if (pthread_create(&notify_thread, &attr, notifyThread, NULL)) return 1;

  // precompute a union of Commons FPs, Wikipedia FPs, QIs, and VIs 
  int goodCats[][3] = { 
    {3943817,0,1}, // [[Category:Featured_pictures_on_Wikimedia_Commons]]     (depth 0)
    {5799448,1,1}, // [[Category:Featured_pictures_on_Wikipedia_by_language]] (depth 1)
    {3618826,0,2}, // [[Category:Quality_images]]                             (depth 0)
    {4143367,0,3}  // [[Category:Valued_images_sorted_by_promotion_date]]     (depth 0)
  };
  goodImages->clear();
  goodImages->addTags();
  result_type r;
  for (int i=4; i>0; --i) {
    printf("goodImages[%d]\n", i);
    result[0]->clear();
    result[0]->num = 0;
    fetchFiles(goodCats[i-1][0], goodCats[i-1][1], result[0]);
    for (int j =0; j<result[0]->num; j++) {
      r = result[0]->buf[j] & cat_mask;
      goodImages->mask[r] = result[0]->mask[r];
      goodImages->tags[r] = goodCats[i-1][2];
    }
  }
  goodImages->num = -1;

  // start webserver
  onion *o=onion_new(O_THREADED);

  onion_set_port(o, argv[1]);
  onion_set_hostname(o,"0.0.0.0");
  onion_set_timeout(o, 1000000000);

  // add handlers
  onion_url *url=onion_root_url(o);
  onion_url_add(url, "status", (void*)handleStatus);
  onion_url_add(url, "",    (void*)handleRequest);

  fprintf(stderr,"Server ready. [%ld,%ld]\n", sizeof(tree_type), sizeof(result_type));
  int error = onion_listen(o);
  if (error) perror("Cant create the server");
  
  onion_free(o);

  free(cat);
  free(tree);
  return 0;
}
