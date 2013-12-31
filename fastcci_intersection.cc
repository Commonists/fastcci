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

// work item status type
enum wiStatus { WI_WAITING, WI_COMPUTING, WI_STREAMING, WI_DONE };

// work item queue
int aItem = 0, bItem = 0;
const int maxItem = 1000;
struct workItem {
  // thread data
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  // response
  onion_response *res;
  // query parameters
  int c1, c2; // categories
  int d1, d2; // depths
  // status
  wiStatus status;
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

void intersect(int c1, int c2) {
  int cid[2] = {c1,c2};

  // generate intermediate results
  for (int i=0; i<(c1!=c2)?2:1; ++i) {
    // clear visitation mask
    memset(mask,0,maxcat);
    
    // fetch files through deep traversal
    resbuf=i;
    fetchFiles(cid[i],0);
    fprintf(stderr,"fnum %d\n", fnum[i]);
  }

  // if the same cat was specified twice, just list all the files
  if (c1==c2) {
    qsort(fbuf[0], fnum[0], sizeof(int), compare);
    int lr=-1;
    for (int i=0; i<fnum[0]; ++i) 
      if (fbuf[0][i]!=lr) {
        // output file
        lr=fbuf[0][i];
      }
    return;
  }

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

    int *j0, *j1, r, *j, *end=&(fbuf[small][fnum[small]+1]);
    for (int i=0; i<fnum[large]; ++i) {
      j = (int*)bsearch((void*)&(fbuf[large][i]), fbuf[small], fnum[small], sizeof(int), compare);
      if (j) {
        // output the result 
        printf("%d\n",fbuf[large][i]);

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
    int i0=0, i1=1, r, lr=-1;
    do {
      if (fbuf[0][i0] < fbuf[1][i1]) 
        i0++;
      else if (fbuf[0][i0] > fbuf[1][i1]) 
        i1++;
      else {
        r = fbuf[0][i0];
        if (r!=lr) printf("%d\n",r);
        lr = r;
        i0++;
        i1++;
      }
    } while (i0 < fnum[0] && i1<fnum[1]);
  }
}

onion_connection_status handleRequest(void *d, onion_request *req, onion_response *res)
{
  // this routine is called by a single thread only (MHD_USE_SELECT_INTERNALLY)

  fprintf(stderr,"Handle Request '%s'\n", onion_request_get_path(req) );

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
  pthread_mutex_unlock(&mutex);

  // new queue item
  int i = bItem % maxItem;

  // store response pointer
  queue[i].res = res;

  queue[i].c1 = atoi(c1);
  queue[i].c2 = c2 ? atoi(c2) : -1;

  const char* d1 = onion_request_get_query(req, "d1");
  const char* d2 = onion_request_get_query(req, "d2");

  queue[i].d1 = d1 ? atoi(d1) : -1;
  queue[i].d2 = d2 ? atoi(d2) : -1;

  queue[i].status = WI_WAITING;

  fprintf(stderr,"End of handle connection (1st call). %x (%d,%d)\n", long(req), aItem, bItem);

  // append to the queue and signal worker thread
  pthread_mutex_lock(&mutex);
  bItem++;
  pthread_cond_signal(&condition);
  pthread_mutex_unlock(&mutex);

  // send keep-alive response
  onion_response_set_header(res,"Transfer-Encoding","Chunked");
  onion_response_write0(res, "Computing..\n");  
  onion_response_flush(res);
  onion_response_flush(res);

  // wait for signal from worker thread (TODO: have a third thread periodically signal, only print result when the calculation is done, otherwise print status)
  bool done;
  do {
    pthread_mutex_lock(&(queue[i].mutex));
    pthread_cond_wait(&(queue[i].cond), &(queue[i].mutex));
    done = (queue[i].status == WI_DONE);
    pthread_mutex_unlock(&(queue[i].mutex));

    if (!done) {
      onion_response_write0(res, "..\n");  
      onion_response_flush(res);
      onion_response_flush(res);
    }
  } while (!done);

  onion_response_write0(res, "Done..\n");  
  onion_response_flush(res);

  fprintf(stderr,"End of handle connection.\n");
  return OCS_CLOSE_CONNECTION;
  //return OCS_KEEP_ALIVE;
}

void *notifyThread( void *d ) {
  while(1) {
    sleep(2);
    
    pthread_mutex_lock(&handlerMutex);
    pthread_mutex_lock(&mutex);
    
    // loop over all active queue items (actually lock &mutex as well!)
    for (int i=aItem; i<bItem; ++i)
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

      // compute result
      fprintf(stderr, "Starting compute\n");
      sleep(10);
      fprintf(stderr, "Completed compute\n");

      queue[i].status = WI_STREAMING;

      // stream response
      onion_response_write0(queue[i].res, "result\n");  
      onion_response_flush(queue[i].res);

      queue[i].status = WI_DONE;

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

  if (argc==3) {
    // commandline test
    intersect(atoi(argv[1]), atoi(argv[2]));
  } else {
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
    //onion_set_hostname(o,"0.0.0.0");
    onion_set_timeout(o, 1000000000);
    onion_set_root_handler(o, onion_handler_new(handleRequest, NULL, NULL) );
    fprintf(stderr,"Server ready.\n");
    int error = onion_listen(o);
    if (error) perror("Cant create the server");
    
    onion_free(o);
    return 0;
  }

  free(cat);
  free(tree);
  free(mask);
  free(fbuf[0]);
  free(fbuf[1]);
  return 0;
}
