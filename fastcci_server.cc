#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#if !defined(__APPLE__)
#include <malloc.h>
#endif
#include <string.h>
#include <pthread.h>

#include <onion/onion.h>
#include <onion/handler.h>
#include <onion/response.h>
#include <onion/websocket.h>

// thread management objects
pthread_mutex_t handlerMutex;
pthread_mutex_t mutex;
pthread_cond_t condition;

// category data and traversal information
const int maxdepth=500;
int resbuf;
int *fbuf[2] = {0}, fmax[2]={100000,100000}, fnum[2];
int *cat, maxcat; 
int *tree; 
char *mask;

// work item type
enum wiConn { WC_XHR, WC_SOCKET };

// work item type
enum wiType { WT_INTERSECT, WT_TRAVERSE, WT_NOTIN };

// work item status type
enum wiStatus { WS_WAITING, WS_PREPROCESS, WS_COMPUTING, WS_STREAMING, WS_DONE };

// work item queue
int aItem = 0, bItem = 0;
const int maxItem = 1000;
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
} queue[maxItem];

int readFile(const char *fname, int* &buf) {
  FILE *in = fopen(fname,"rb");
  fseek(in, 0L, SEEK_END);
  int sz = ftell(in);
  fseek(in, 0L, SEEK_SET);
  buf = (int*)malloc(sz);
  fread(buf, 1, sz, in);
  return sz;
}

void fetchFiles(int id, int depth) {
  // record path
  if (depth==maxdepth) return;

  // previously visited category
  int i;
  if (mask[id] != 0) return;

  // mark as visited
  mask[id]=1;
  int c = cat[id], cend = tree[c], cfile = tree[c+1];
  c += 2;
  while (c<cend) {
    fetchFiles(tree[c], depth+1);
    c++; 
  }

  // copy files
  int len = cfile-c;
  if (fnum[resbuf]+len > fmax[resbuf]) {
    // grow buffer
    while (fnum[resbuf]+len > fmax[resbuf]) fmax[resbuf] *= 2;
    fbuf[resbuf] = (int*)realloc(fbuf[resbuf], fmax[resbuf]*sizeof(int));
  }
  memcpy(&(fbuf[resbuf][fnum[resbuf]]), &(tree[c]), sizeof(int)*len);
  fnum[resbuf] += len;
}

int compare (const void * a, const void * b) {
  return ( *(int*)a - *(int*)b );
}

void traverse(int qi) {
  int n = 0; // number of current output item

  int outstart = queue[qi].o;
  int outend   = outstart + queue[qi].s;
  onion_response *res = queue[qi].res;
  onion_websocket *ws = queue[qi].ws;

  // sort
  qsort(fbuf[0], fnum[0], sizeof(int), compare);

  // output unique files
  queue[qi].status = WS_STREAMING;
  int lr=-1;
  for (int i=0; i<fnum[0]; ++i) 
    if (fbuf[0][i]!=lr) {
      n++;
      // are we still below the offset?
      if (n<=outstart) continue;
      // output file      
      lr=fbuf[0][i];
      if (res) onion_response_printf(res, "%d\n", lr);
      if (ws)  onion_websocket_printf(ws, "%d\n", lr);
      // are we at the end of the output window?
      if (n>=outend) break;
    }
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
  qsort(fbuf[0], fnum[0], sizeof(int), compare);
  qsort(fbuf[1], fnum[1], sizeof(int), compare);
/*
  1 2
  3 3
  4 5
  8 8
  9 
*/

  // perform subtraction
  int i0=0, i1=0, r, lr=-1;
  queue[qi].status = WS_STREAMING;
  do {
    if (fbuf[0][i0] < fbuf[1][i1]) {
      r = fbuf[0][i0];
      
      if (r!=lr) {
        // are we at the output offset?
        if (n>=outstart) {
          if (res) onion_response_printf(res, "%d\n", r);
          if (ws)  onion_websocket_printf(ws, "%d\n", r);
        }
        n++;
        if (n>=outend) break;
      }

      lr = r;

      // advance i0 until we are at a different entry
      for(; i0<fnum[0] && fbuf[0][i0]==r; i0++);
    } else if (fbuf[0][i0] > fbuf[1][i1]) { 
      r = fbuf[1][i1];

      // advance i1 until we are at a different entry
      for(; i1<fnum[1] && fbuf[1][i1]==r; i1++);
    } else { // equal
      // advance i0 until we are at a different entry
      r = fbuf[0][i0];
      for(; i0<fnum[0] && fbuf[0][i0]==r; i0++);

      // advance i1 until we are at a different entry
      r = fbuf[1][i1];
      for(; i1<fnum[1] && fbuf[1][i1]==r; i1++);
    }
  } while (i0 < fnum[0] && i1<fnum[1]);

  // dump the remainder of c1
  if (i0 < fnum[0] && i1>=fnum[1]) {
    for (;i0 < fnum[0]; ++i0) {
      r = fbuf[0][i0];
      
      if (r!=lr) {
        // are we at the output offset?
        if (n>=outstart) {
          if (res) onion_response_printf(res, "%d\n", r);
          if (ws)  onion_websocket_printf(ws, "%d\n", r);
        }
        n++;
        if (n>=outend) break;
      }

      lr = r;
    }
  }
}

void intersect(int qi) {
  int cid[2] = {queue[qi].c1, queue[qi].c2};
  int n = 0; // number of current output item

  int outstart = queue[qi].o;
  int outend   = outstart + queue[qi].s;
  onion_response *res = queue[qi].res;
  onion_websocket *ws = queue[qi].ws;

  // decide on an intersection strategy
  if (fnum[0]>1000000 || fnum[1]>1000000) {
    fprintf(stderr,"using bsearch strategy.\n");
    // sort the smaller and bsearch on it
    int small, large;
    if (fnum[0] < fnum[1]) {
      small=0; large=1; 
    } else {
      small=1; large=0; 
    }
    qsort(fbuf[small], fnum[small], sizeof(int), compare);

    queue[qi].status = WS_STREAMING;
    int *j0, *j1, r, *j, *end=&(fbuf[small][fnum[small]+1]);
    for (int i=0; i<fnum[large]; ++i) {
      j = (int*)bsearch((void*)&(fbuf[large][i]), fbuf[small], fnum[small], sizeof(int), compare);
      if (j) {
        // are we at the output offset?
        if (n>=outstart) {
          if (res) onion_response_printf(res, "%d\n", fbuf[large][i]);
          if (ws)  onion_websocket_printf(ws, "%d\n", fbuf[large][i]);
        }
        n++;
        if (n>=outend) break;

        // remove this match from the small result set
        j0=j; while(j0>fbuf[small] && *j==*j0) j0--; 
        j1=j; while(j1<end && *j==*j1) j1++;

        // fill in from the entry before or after (if this was the last entry break out of the loop)
        if (j1<end) r=*j1;
        else if (j0>fbuf[small]) r=*j0;
        else break;

        j1--;
        do {
          *(++j0)=r;
        } while(j0<j1);
      }
    }
  } else {
    // sort both and intersect then
    fprintf(stderr,"using sort strategy.\n");
    qsort(fbuf[0], fnum[0], sizeof(int), compare);
    qsort(fbuf[1], fnum[1], sizeof(int), compare);

    // perform intersection
    queue[qi].status = WS_STREAMING;
    int i0=0, i1=0, r, lr=-1;
    do {
      if (fbuf[0][i0] < fbuf[1][i1]) 
        i0++;
      else if (fbuf[0][i0] > fbuf[1][i1]) 
        i1++;
      else {
        r = fbuf[0][i0];
        
        if (r!=lr) {
          // are we at the output offset?
          if (n>=outstart) {
            if (res) onion_response_printf(res, "%d\n", r);
            if (ws)  onion_websocket_printf(ws, "%d\n", r);
          }
          n++;
          if (n>=outend) break;
        }

        lr = r;
        i0++;
        i1++;
      }
    } while (i0 < fnum[0] && i1<fnum[1]);
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

int queueRequest(onion_request *req)
{
  // parse parameters
  const char* c1 = onion_request_get_query(req, "c1");
  const char* c2 = onion_request_get_query(req, "c2");

  if (c1==NULL) {
    // must supply c1 parameter!
    fprintf(stderr,"No c1 parameter.\n");
    return -1;
  }

  // still room on the queue?
  pthread_mutex_lock(&mutex);
  if( bItem-aItem+1 >= maxItem ) {
    // too many requests. reject
    fprintf(stderr,"Queue full.\n");
    pthread_mutex_unlock(&mutex);
    return -1;
  }
  pthread_mutex_unlock(&mutex);

  // new queue item
  int i = bItem % maxItem;

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
      else
        return OCS_INTERNAL_ERROR;
    }
  }

  return i;
}

onion_connection_status handleRequestXHR(void *d, onion_request *req, onion_response *res)
{
  // try to queue the request
  int i=queueRequest(req);
  if (i<0) return OCS_INTERNAL_ERROR;

  // store response pointer
  queue[i].connection = WC_XHR;
  queue[i].res = res;
  queue[i].ws  = NULL;

  // send keep-alive response
  onion_response_set_header(res, "Access-Control-Allow-Origin", "*");
  onion_response_set_header(res, "Content-Type","text/plain; charset=utf8");

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

  return OCS_CLOSE_CONNECTION;
}

onion_connection_status handleRequestSocket(void *d, onion_request *req, onion_response *res)
{
  // try to queue the request
  int i=queueRequest(req);
  if (i<0) return OCS_INTERNAL_ERROR;

  fprintf(stderr,"before ws new\n");

  // store websocket data
  onion_websocket *ws = onion_websocket_new(req, res);
  fprintf(stderr,"after ws new %lx\n", long(ws));
  if (!ws) return OCS_INTERNAL_ERROR;
  queue[i].connection = WC_SOCKET;
  queue[i].ws  = ws;
  queue[i].res = NULL;

  fprintf(stderr,"append\n");

  onion_websocket_printf(ws, "QUEUED %d\n", i-aItem);  
  // append to the queue and signal worker thread

  pthread_mutex_lock(&mutex);
  bItem++;
  pthread_cond_signal(&condition);
  pthread_mutex_unlock(&mutex);
  fprintf(stderr,"append done\n");

  // wait for signal from worker thread (have a third thread periodically signal, only print result when the calculation is done, otherwise print status)
  wiStatus status;
  do {
    pthread_mutex_lock(&(queue[i].mutex));
  fprintf(stderr,"locked\n");
    pthread_cond_wait(&(queue[i].cond), &(queue[i].mutex));
  fprintf(stderr,"wait done\n");
    status = queue[i].status;
    pthread_mutex_unlock(&(queue[i].mutex));
  fprintf(stderr,"unlocked\n");

    fprintf(stderr,"notify status %d\n", status);
    switch (status) {
      case WS_WAITING :
        // send number of jobs ahead of this one in queue
        onion_websocket_printf(ws, "WAITING %d\n", i-aItem);  
        break;
      case WS_PREPROCESS :
      case WS_COMPUTING :
        // send intermediate result sizes
        onion_websocket_printf(ws, "WORKING %d %d\n", fnum[0], fnum[1]);  
        break;
    }
    
    // don't do anything if status is WS_STREAMING, the compute task is sending data

  } while (status != WS_DONE);

  onion_websocket_printf(ws, "DONE\n");  

  fprintf(stderr,"End of handle connection.\n");
  return OCS_CLOSE_CONNECTION;
  //return OCS_WEBSOCKET;
}

void *notifyThread( void *d ) {
  while(1) {
    sleep(2);
    
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
      if (queue[i].ws ) onion_websocket_printf(queue[i].ws, "COMPUTE_START\n");  

      fnum[0] = 0;
      fnum[1] = 0;
      queue[i].status = WS_PREPROCESS;

      // generate intermediate results
      int cid[2] = {queue[i].c1, queue[i].c2};
      for (int i=0; i<((cid[0]!=cid[1])?2:1); ++i) {
        // clear visitation mask
        memset(mask,0,maxcat);
        
        // fetch files through deep traversal
        resbuf=i;
        fetchFiles(cid[i],0);
        fprintf(stderr,"fnum(%d) %d\n", cid[i], fnum[i]);
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


      queue[i].status = WS_DONE;

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
  maxcat /= sizeof(int);
  mask = (char*)malloc(maxcat);

  readFile("../fastcci.tree", tree);

  // intermediate return buffers
  fbuf[0]=(int*)malloc(fmax[0]*sizeof(int));
  fbuf[1]=(int*)malloc(fmax[1]*sizeof(int));

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
  onion_set_hostname(o,"::");
  onion_set_timeout(o, 1000000000);
  //onion_set_root_handler(o, onion_handler_new(handleRequest, NULL, NULL) );

  // add handlers
  onion_url *url=onion_root_url(o);
  onion_url_add(url, "status", (void*)handleStatus);
  onion_url_add(url, "xhr",    (void*)handleRequestXHR);
  onion_url_add(url, "socket", (void*)handleRequestSocket);


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
