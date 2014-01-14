#include <time.h>
#include "fastcci.h"

// thread management objects
pthread_mutex_t handlerMutex;
pthread_mutex_t mutex;
pthread_cond_t condition;

// category data and traversal information
const int maxdepth=500;
int resbuf;
int fmax[2]={100000,100000}, fnum[2];
result_type *fbuf[2] = {0};
int maxcat; 
tree_type *cat, *tree, *parent; 
char *mask;

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

/*
 * Fetch all files in and below category 'id'
 * in a breadth first search up to depth 'depth'
 * if 'depth' id negative treat it as infinity
 */
void fetchFiles(int id, int depth) {
  // clear ring buffer
  rbClear(rb);

  // compute max depth
  result_type md = result_type(depth) << depth_shift;

  // push root node (depth 0)
  rbPush(rb,id);

  result_type r, d,e, i;
  int c, len;
  while (!rbEmpty(rb)) {
    r = rbPop(rb);
    d = r & depth_mask;
    i = r & cat_mask;
    
    // tag current category as visited
    mask[i]=1;

    int c = cat[i], cend = tree[c], cfile = tree[c+1];
    c += 2;

    // push all subcat to queue
    e = d + (1l<<depth_shift);
    if (d<md || depth<0) {
      while (c<cend) {
        // push unvisited categories into the queue
        if (mask[tree[c]]==0) rbPush(rb, tree[c] | e);
        c++;
      }
    }

    // copy and add the depth on top
    int len = cfile-c;
    if (fnum[resbuf]+len > fmax[resbuf]) {
      // grow buffer
      while (fnum[resbuf]+len > fmax[resbuf]) fmax[resbuf] *= 2;
      fbuf[resbuf] = (result_type*)realloc(fbuf[resbuf], fmax[resbuf] * sizeof **fbuf);
    }
    result_type *dst = &(fbuf[resbuf][fnum[resbuf]]), *old = dst;
    tree_type   *src = &(tree[c]);
    while (len--) {
      r =  *src++; 
      if (mask[r]==0) {
        *dst++ = (r | d);
        mask[r] = 2;
      }
    }
    fnum[resbuf] += dst-old;
  }
}

// recursively tag categories to find a path between Category -> File  or  Category -> Category
int history[maxdepth];
bool found;
void tagCat(int id, int qi, int depth) {
  // record path
  if (depth==maxdepth || mask[id]!=0 || found) return;
  history[depth] = id;

  int c = cat[id], cend = tree[c], cend2 = tree[c+1];
  bool foundPath = false;
  if (id==queue[qi].c2) foundPath=true;
  // check if c2 is a file (cat[c2]<0)
  else if (cat[queue[qi].c2]<0) {
    // if so, search the files in this cat
    for (int i=cend; i<cend2; ++i) {
      if (tree[i]==queue[qi].c2) {
        foundPath=true;
        break;
      }
    }
  }

  // found the target category
  if (foundPath) {
    for (int i=0; i<=depth; ++i)
      resultQueue(qi, history[i]);
    resultFlush(qi);
    found = true;
    return;
  }

  // mark as visited
  mask[id]=1;
  c += 2;
  while (c<cend) {
    tagCat(tree[c], qi, depth+1);
    c++; 
  }
}
// iteratively do a breadth first path search
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
    d  = r & depth_mask;
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
      e = d + (1l<<depth_shift);
    if (depth<maxDepth || maxDepth<0) {
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

void traverse(int qi) {
  int n = 0; // number of current output item

  int outstart = queue[qi].o;
  int outend   = outstart + queue[qi].s;
  onion_response *res = queue[qi].res;
  onion_websocket *ws = queue[qi].ws;

  // sort
  qsort(fbuf[0], fnum[0], sizeof **fbuf, compare);

  // output unique files
  queue[qi].status = WS_STREAMING;
  result_type lr=-1, r;
  for (int i=0; i<fnum[0]; ++i) {
    r = fbuf[0][i] & cat_mask;
    if (r!=lr) {
      n++;
      // are we still below the offset?
      if (n<=outstart) continue;
      // output file      
      lr=r;
      resultQueue(qi, fbuf[0][i]);
      // are we at the end of the output window?
      if (n>=outend) break;
    }
  }
  resultFlush(qi);

  // send the (exact) size of the complete result set
  resultPrintf(qi, "OUTOF %d", fnum[0]); 
}

void notin(int qi) {
  int cid[2] = {queue[qi].c1, queue[qi].c2};
  int n = 0; // number of current output item

  int outstart = queue[qi].o;
  int outend   = outstart + queue[qi].s;
  onion_response *res = queue[qi].res;
  onion_websocket *ws = queue[qi].ws;

  // sort both and subtract then
  fprintf(stderr,"using sort strategy.\n");
  qsort(fbuf[0], fnum[0], sizeof **fbuf, compare);
  qsort(fbuf[1], fnum[1], sizeof **fbuf, compare);
/*
  1 2
  3 3
  4 5
  8 8
  9 
*/

  // perform subtraction
  int i0=0, i1=0;
  tree_type r, lr;
  result_type *j0 = fbuf[0], 
              *j1 = fbuf[1];
  result_type *f0 = &(fbuf[0][fnum[0]]), 
              *f1 = &(fbuf[1][fnum[1]]);

  queue[qi].status = WS_STREAMING;
  do {
    //if (fbuf[0][i0] < fbuf[1][i1]) {
    if ( *(tree_type*)j0 > *(tree_type*)j1 ) {
      r = *(tree_type*)j0; // = fbuf[0][i0] & cat_mask;
      
      if (r!=lr) {
        // are we at the output offset?
        //if (n>=outstart) resultQueue(qi, fbuf[0][i0]);
        if (n>=outstart) resultQueue(qi, *j0);
        n++;
        if (n>=outend) break;
      }

      lr = r;

      // advance i0 until we are at a different entry
      for(; j0<f0 && *(tree_type*)j0==r; j0++);
    } else if (*(tree_type*)j0 < *(tree_type*)j1) { //    fbuf[0][i0] > fbuf[1][i1]) { 
      r = *(tree_type*)j1; // = fbuf[1][i1] & cat_mask;

      // advance i1 until we are at a different entry
      //for(; i1<fnum[1] && (fbuf[1][i1] & cat_mask)==r; i1++);
      for(; j1<f1 && *(tree_type*)j1==r; j1++);
    } else { // equal
      // advance i0 until we are at a different entry
      r = *(tree_type*)j0;
      for(; j0<f0 && *(tree_type*)j0==r; j0++);

      // advance i1 until we are at a different entry
      r = *(tree_type*)j1;
      for(; j1<f1 && *(tree_type*)j1==r; j1++);
    }
  } while (j0<f0 && j1<f1);

  // dump the remainder of c1
  if (j0<f0 && j1>=f1) {
    for (;j0<f0; ++j0) {
      r = *(tree_type*)j0;
      
      if (r!=lr) {
        // are we at the output offset?
        if (n>=outstart) resultQueue(qi, *j0);
        n++;
        if (n>=outend) break;
      }

      lr = r;
    }
  }

  resultFlush(qi);
}

void intersect(int qi) {
  int cid[2] = {queue[qi].c1, queue[qi].c2};
  int n = 0; // number of current output item

  int outstart = queue[qi].o;
  int outend   = outstart + queue[qi].s;
  onion_response *res = queue[qi].res;
  onion_websocket *ws = queue[qi].ws;

  // was one of the results empty?
  if (fnum[0]==0 || fnum[1]==0) {
    resultPrintf(qi, "OUTOF %d", 0); 
    return;
  }

  // otherwise decide on an intersection strategy
  if (fnum[0]>1000000 || fnum[1]>1000000) {
    fprintf(stderr,"using bsearch strategy.\n");
    // sort the smaller and bsearch on it
    int small, large;
    if (fnum[0] < fnum[1]) {
      small=0; large=1; 
    } else {
      small=1; large=0; 
    }
    qsort(fbuf[small], fnum[small], sizeof **fbuf, compare);
    // since a breadth first search is used on the search results the low depth results come first in the large results (resulting in the correct minimal depth metric)

    queue[qi].status = WS_STREAMING;
    int i;
    tree_type r;
    result_type *j0, *j1, *j, *end=&(fbuf[small][fnum[small]+1]);
    for (i=0; i<fnum[large]; ++i) {
      j = (result_type*)bsearch((void*)&(fbuf[large][i]), fbuf[small], fnum[small], sizeof **fbuf, compare);
      if (j) {
        // remove this match from the small result set (cast result_type* to tree_type* before dereferencing to only compare the first 32bit)
        // and find minimum depth value. Note: we can only take the depth of the smaller result set into account here!!
        result_type mindepthresult = *j;
        j0=j-1; while(j0>fbuf[small] && *(tree_type*)j==*(tree_type*)j0) {
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
          resultQueue(qi, (fbuf[large][i] & depth_mask) + mindepthresult ); // output result with mindepth plus depth in the large category
        }
        n++;
        if (n>=outend) break;

        // fill in from the entry before or after (if this was the last entry break out of the loop)
        result_type rr;
        if (j1<end) rr = *j1;
        else if (j0>=fbuf[small]) rr = *j0;
        else break;

        j1--;
        while(j0<j1) *(++j0) = rr; 
      }
    }

    resultFlush(qi);

    // send the (estimated) size of the complete result set
    int est = n + int( double(n)/double(i+1) * double(fnum[large]+1) );
    resultPrintf(qi, "OUTOF %d", est); 
  } else {
    // sort both and intersect then
    fprintf(stderr,"using sort strategy.\n");
    qsort(fbuf[0], fnum[0], sizeof **fbuf, compare);
    qsort(fbuf[1], fnum[1], sizeof **fbuf, compare);

    // perform intersection
    queue[qi].status = WS_STREAMING;
    result_type *j0 = fbuf[0], 
                *j1 = fbuf[1];
    result_type *f0 = &(fbuf[0][fnum[0]]), 
                *f1 = &(fbuf[1][fnum[1]]);
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
          onion_websocket_printf(ws, "WORKING %d %d", fnum[0], fnum[1]);  
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
        memset(mask,0,maxcat);
        tagCatNew(queue[i].c1, i, queue[i].d1);
        //tagCat(queue[i].c1, i, 0);
      } else {
        // boolean operations (AND, LIST, NOTIN)
        fnum[0] = 0;
        fnum[1] = 0;
        queue[i].status = WS_PREPROCESS;

        // generate intermediate results
        int cid[2]   = {queue[i].c1, queue[i].c2};
        int depth[2] = {queue[i].d1, queue[i].d2};
        for (int j=0; j<((cid[0]!=cid[1])?2:1); ++j) {
          // clear visitation mask
          memset(mask,0,maxcat);
          
          // fetch files through deep traversal
          resbuf=j;
          fetchFiles(cid[j],depth[j]);
          fprintf(stderr,"fnum(%d) %d\n", cid[j], fnum[j]);

          // check results
          /*for (int k=0; k<fnum[j]; ++k) {
            if (((fbuf[j][k]&depth_mask)>>depth_shift)>1000 )
              fprintf(stderr,"BIGMSK %d\n", (fbuf[j][k]&depth_mask)>>depth_shift );
          }*/
        }

        // compute result
        queue[i].status = WS_COMPUTING;

        switch (queue[i].type) {
          case WT_TRAVERSE :
            traverse(i);
            break;
          case WT_NOTIN :
            notin(i);
            break;
          case WT_INTERSECT :
            intersect(i);
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

  maxcat = readFile("../fastcci.cat", cat);
  maxcat /= sizeof(tree_type);

  // visitation mask buffer (could be 1/8 by using a bitmask)
  mask = (char*)malloc(maxcat);

  // parent category buffer for shortest path finding
  parent = (tree_type*)malloc(maxcat * sizeof *parent);

  readFile("../fastcci.tree", tree);

  // intermediate return buffers
  fbuf[0]=(result_type*)malloc(fmax[0] * sizeof **fbuf);
  fbuf[1]=(result_type*)malloc(fmax[1] * sizeof **fbuf);

  // ring buffer for breadth first
  rbInit(rb);

  if (argc != 2) {
    printf("%s PORT\n", argv[0]);
    return 1;
  }
  
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

  fprintf(stderr,"Server ready.\n");
  int error = onion_listen(o);
  if (error) perror("Cant create the server");
  
  onion_free(o);

  free(cat);
  free(tree);
  free(mask);
  free(fbuf[0]);
  free(fbuf[1]);
  return 0;
}
