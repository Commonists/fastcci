#include <time.h>
#include "fastcci.h"

// thread management objects
pthread_mutex_t handlerMutex;
pthread_mutex_t mutex;
pthread_cond_t condition;

// category data and traversal information
const int maxdepth=500;
int maxcat; 
tree_type *cat, *tree, *parent; 
unsigned char *mask;

// new result data structure
struct resultList {
  int max, num;
  result_type *buf;
  resultList() : max(100000), num(0) {
    buf = (result_type*)malloc(max * sizeof *buf);
  }

  // pointer to element after the last result
  result_type* tail() const { return &(buf[num]); }  

  // grow buffer to hold at least len more items
  void grow(int len) {
    if (num+len > max) {
      while (num+len > max) max *= 2;
      buf = (result_type*)realloc(buf, max * sizeof *buf);
    }
  }

  // sort result list
  void sort() {
    qsort(buf, num, sizeof *buf, compare);
  }
} result[2], goodImages;

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
const int resmaxqueue = 50, resmaxbuf = 32*resmaxqueue;
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
void resultQueue(int i, result_type item) {
  // TODO check for truncation (but what then?!)
  residx += snprintf(&(rescombuf[residx]), resmaxbuf-residx, "%d,%d|", int(item & cat_mask), int((item & depth_mask)>>depth_shift));

  // queued enough values?
  if (++resnumqueue == resmaxqueue) resultFlush(i);
}


//
// Fetch all files in and below category 'id'
// in a breadth first search up to depth 'depth'
// if 'depth' id negative treat it as infinity
//
void fetchFiles(tree_type id, int depth, resultList &result) {
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
    
    /*if (cat[i]<0) 
      fprintf(stderr,"Ring buffer popped file! %d\n", i);
    else
      fprintf(stderr,"OK %d\n", i);*/

    // tag current category as visited
    mask[i]=1;

    int c = cat[i], cend = tree[c], cfile = tree[c+1];
    c += 2;
    //fprintf(stderr,"C %d %d %d\n", c, cend, cfile);

    // push all subcat to queue
    if (d<depth || depth<0) {
      e = (d+1)<<depth_shift;
      while (c<cend) {
        // push unvisited categories (that are not empty, cat[id]==0) into the queue
        if (mask[tree[c]]==0 && cat[tree[c]]>0) {
          rbPush(rb, tree[c] | e);
        }
        c++;
      }
    }

    // copy and add the depth on top
    int len = cfile-c;
    result.grow(len);
    result_type *dst = result.tail(), *old = dst;
    tree_type   *src = &(tree[c]);
    f = d<254 ? (d+1) : 255;
    while (len--) {
      r =  *src++; 
      if (mask[r]==0) {
        *dst++ = (r | d);
        mask[r] = f;
      }
    }
    result.num += dst-old;
  }
}

//
// iteratively do a breadth first path search
//
result_type history[maxdepth]; 
void tagCatNew(tree_type sid, int qi, int maxDepth) {
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
        if (mask[tree[c]]==0) {
          parent[tree[c]] = id;
          mask[tree[c]]   = 1;
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
    while (i--) resultQueue(qi, history[i] + (result_type(len-i)<<depth_shift));
    resultFlush(qi);
  } else {
    resultPrintf(qi, "NOPATH"); 
  }
}

//
// all images in result
//
void traverse(int qi, resultList &result) {
  int outstart = queue[qi].o;
  int outend   = outstart + queue[qi].s;
  onion_response *res = queue[qi].res;
  onion_websocket *ws = queue[qi].ws;

  // sort
  result.sort();

  // output selected subset
  queue[qi].status = WS_STREAMING;
  if (outend>result.num) outend=result.num;
  for (int i=outstart; i<outend; ++i)
    resultQueue(qi, result.buf[i]);
  resultFlush(qi);

  // send the (exact) size of the complete result set
  resultPrintf(qi, "OUTOF %d", result.num); 
}

//
// all images in result that are not flagged in the mask
//
void notin(int qi, resultList &result) {
  int cid[2] = {queue[qi].c1, queue[qi].c2};
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
  for (int i=0; i<result.num; ++i) {
    r = result.buf[i] & cat_mask;

    if (mask[r]==0) {
      n++;
      // are we still below the offset?
      if (n<=outstart) continue;
      // output file      
      resultQueue(qi, result.buf[i]);
      // are we at the end of the output window?
      if (n>=outend) break;
    }
  }

  resultFlush(qi);
}

//
// all images in both rl0 and rl1 TODO: split sort and bsearch into separate functions. replace bsearch with mask-based approach(?)
//
void intersect(int qi, resultList &rl0, resultList &rl1) {
  int cid[2] = {queue[qi].c1, queue[qi].c2};
  resultList result[] = { rl0, rl1 };
  int n = 0; // number of current output item

  int outstart = queue[qi].o;
  int outend   = outstart + queue[qi].s;
  onion_response *res = queue[qi].res;
  onion_websocket *ws = queue[qi].ws;

  // was one of the results empty?
  if (result[0].num==0 || result[1].num==0) {
    resultPrintf(qi, "OUTOF %d", 0); 
    return;
  }

  // otherwise decide on an intersection strategy
  if (result[0].num>1000000 || result[1].num>1000000) {
    fprintf(stderr,"using bsearch strategy.\n");
    // sort the smaller and bsearch on it
    int small, large;
    if (result[0].num < result[1].num) {
      small=0; large=1; 
    } else {
      small=1; large=0; 
    }
    
    result[small].sort();
    // since a breadth first search is used on the search results the low depth results come first in the large results (resulting in the correct minimal depth metric)

    queue[qi].status = WS_STREAMING;
    int i;
    tree_type r;
    result_type *j0, *j1, *j, *end=result[small].tail(); //&(fbuf[small][fnum[small]+1]); WHY +1?
    for (i=0; i<result[large].num; ++i) {
      j = (result_type*)bsearch((void*)&(result[large].buf[i]), result[small].buf, result[small].num, sizeof(result_type), compare);
      if (j) {
        // TODO: mindepth search is not necessary anymore, because results are unique and minimal due to masking

        // remove this match from the small result set (cast result_type* to tree_type* before dereferencing to only compare the first 32bit)
        // and find minimum depth value. Note: we can only take the depth of the smaller result set into account here!!
        result_type mindepthresult = *j;
        j0=j-1; while(j0>result[small].buf && *(tree_type*)j==*(tree_type*)j0) {
          if ((*j0 & depth_mask) < (mindepthresult & depth_mask) ) mindepthresult = *j0;
          j0--; 
        }
        j1=j+1; while(j1<end && *(tree_type*)j==*(tree_type*)j1) {
          if ((*j1 & depth_mask) < (mindepthresult & depth_mask) ) mindepthresult = *j1;
          j1++;
        }

        // are we at the output offset?
        if (n>=outstart) {
          //fprintf(stderr, "depth: %ld %ld  %d %d\n", fbuf[large][i], mindepthresult, (fbuf[large][i] & depth_mask)>>depth_shift, (mindepthresult & depth_mask)>>depth_shift);
          resultQueue(qi, (result[large].buf[i] & depth_mask) + mindepthresult ); // output result with mindepth plus depth in the large category
        }
        n++;
        if (n>=outend) break;

        // fill in from the entry before or after (if this was the last entry break out of the loop)
        result_type rr;
        if (j1<end) rr = *j1;
        else if (j0>=result[small].buf) rr = *j0;
        else break;

        j1--;
        while(j0<j1) *(++j0) = rr; 
      }
    }

    resultFlush(qi);

    // send the (estimated) size of the complete result set
    int est = n + int( double(n)/double(i+1) * double(result[large].num+1) );
    resultPrintf(qi, "OUTOF %d", est); 
  } else {
    // sort both and intersect then
    fprintf(stderr,"using sort strategy.\n");
    result[0].sort();
    result[1].sort();

    // perform intersection
    queue[qi].status = WS_STREAMING;
    result_type *j0 = result[0].buf, 
                *j1 = result[1].buf;
    result_type *f0 = result[0].tail(),
                *f1 = result[1].tail(); 
    tree_type r, lr=-1;
    result_type m0, m1;
    do {
      if (*(tree_type*)j0 > *(tree_type*)j1) 
        j0++;
      else if (*(tree_type*)j0 < *(tree_type*)j1) 
        j1++;
      else {
        m0 = (*j0++);
        m1 = (*j1++) & depth_mask;
 
        // are we at the output offset?
        if (n>=outstart) resultQueue(qi, m0+m1);
        n++;
        if (n>=outend) break;
      }
    } while (j0<f0 && j1<f1);

    resultFlush(qi);

    // send the (estimated) size of the complete result set
    /*int s = n-outstart;
    int est1 = n + int( double(n)/double(i0+1) * double(fnum[0]+1) );
    int est2 = n + int( double(n)/double(i1+1) * double(fnum[1]+1) );
    resultPrintf(qi, "OUTOF %d\n", est1<est2?est1:est2); */
  }
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

  if (queue[i].c1==queue[i].c2)
    queue[i].type = WT_TRAVERSE;
  else {
    queue[i].type = WT_INTERSECT;

    if (aparam != NULL) {
      if (strcmp(aparam,"and")==0)
        queue[i].type = WT_INTERSECT;
      else if (strcmp(aparam,"not")==0)
        queue[i].type = WT_NOTIN;
      else if (strcmp(aparam,"list")==0)
        queue[i].type = WT_TRAVERSE;
      else if (strcmp(aparam,"path")==0) {
        queue[i].type = WT_PATH;
        if (queue[i].c1==queue[i].c2) return OCS_INTERNAL_ERROR;
      }
      else
        return OCS_INTERNAL_ERROR;
    }
  }

  // check if invalid ids were specified
  if (queue[i].c1>=maxcat || queue[i].c2>=maxcat || 
      queue[i].c1<0 || queue[i].c2<0) return OCS_INTERNAL_ERROR;

  // check if both c params are categories unless it is a path request
  if (isFile(queue[i].c1) || (isFile(queue[i].c2) && queue[i].type!=WT_PATH) ) return OCS_INTERNAL_ERROR;

  // log request
  if (queue[i].c1==queue[i].c2) aparam="list";
  else if (aparam==NULL) aparam="and";
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
          onion_websocket_printf(ws, "WORKING %d %d", result[0].num, result[1].num);  
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

      if (queue[i].type==WT_PATH) {
        // path finding
        memset(mask,0,maxcat * sizeof *mask);
        tagCatNew(queue[i].c1, i, queue[i].d1);
        //tagCat(queue[i].c1, i, 0);
      } else {
        // boolean operations (AND, LIST, NOTIN)
        result[0].num = 0;
        result[1].num = 0;
        queue[i].status = WS_PREPROCESS;

        // generate intermediate results
        int cid[2]   = {queue[i].c1, queue[i].c2};
        int depth[2] = {queue[i].d1, queue[i].d2};
        for (int j=0; j<((cid[0]!=cid[1] && cid[1]>=0)?2:1); ++j) {
          // clear visitation mask
          memset(mask,0,maxcat * sizeof *mask);
          
          // fetch files through deep traversal
          fetchFiles(cid[j], depth[j], result[j]);
          fprintf(stderr,"fnum(%d) %d\n", cid[j], result[j].num);

          // check results
          /*for (int k=0; k<fnum[j]; ++k) {
            if ( cat[fbuf[j][k]&cat_mask]>=0 )
              fprintf(stderr,"CATINRES %d %d\n", j, k );
          }*/
        }

        // compute result
        queue[i].status = WS_COMPUTING;

        switch (queue[i].type) {
          case WT_TRAVERSE :
            traverse(i, result[0]);
            break;
          case WT_NOTIN :
            notin(i, result[0]);
            break;
          case WT_INTERSECT :
            if (cid[1]==-1)
              intersect(i, result[0], goodImages);
            else
              intersect(i, result[0], result[1]);
            break;
        }
      }

      // done with this request
      resultDone(i);

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

  const int buflen = 1000;
  char fname[buflen];

  snprintf(fname, buflen, "%s/fastcci.cat", argv[2]);
  maxcat = readFile(fname, cat);
  maxcat /= sizeof(tree_type);

  // visitation mask buffer (could be 1/8 by using a bitmask)
  mask = (unsigned char*)malloc(maxcat * sizeof *mask);

  // union of FP/QI/VI
  memset(mask,0,maxcat * sizeof *mask);
  fetchFiles(3943817, 0, goodImages); // FPs
  fetchFiles(3618826, 0, goodImages); // QIs
  fetchFiles(3862724, 0, goodImages); // VIs

  // parent category buffer for shortest path finding
  parent = (tree_type*)malloc(maxcat * sizeof *parent);

  snprintf(fname, buflen, "%s/fastcci.tree", argv[2]);
  readFile(fname, tree);

  // ring buffer for breadth first
  rbInit(rb);

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
  free(mask);
  return 0;
}
