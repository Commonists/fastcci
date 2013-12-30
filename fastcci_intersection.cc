#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#if !defined(__APPLE__)
#include <malloc.h>
#endif
#include <string.h>
#include <pthread.h>

// thread management objects
pthread_mutex_t mutex;
pthread_cond_t condition;

// category data and traversal information
const int maxdepth=500;
int resbuf;
int *fbuf[2] = {0}, fmax[2]={100000,100000}, fnum[2];
int *cat, maxcat; 
int *tree; 
char *mask;

// work item queue
int aItem = 0, bItem = 0;
const int maxItem = 1000;
struct workItem {
  // connection for this work item
  struct MHD_Connection *connection;
  // query parameters
  int c1, c2; // categories
  int d1, d2; // depths
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

int handleRequest(void *cls, struct MHD_Connection *connection, 
                  const char *url, 
                  const char *method, const char *version, 
                  const char *upload_data, 
                  size_t *upload_data_size, void **con_cls)
{
  // this routine is called by a single thread only (MHD_USE_SELECT_INTERNALLY)

  // only accept GET requests
  if (0 != strcmp(method, "GET")) return MHD_NO;
  
  // only accept url '/' requests
  if (0 != strcmp(url, "/")) return MHD_NO;

  fprintf(stderr,"Handle Request '%s' (%x,%x) size=%d.\n",url, long(connection),long(*con_cls), *upload_data_size);

  // first request of the connection
  if (*con_cls == NULL) { 
    *con_cls = connection; 

    // parse parameters
    const char* c1 = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "c1");
    const char* c2 = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "c2");

    if (c1==NULL) {
      // must supply c1 parameter!
      fprintf(stderr,"No c1 parameter.\n");
      return MHD_NO;
    }

    // still room on the queue?
    pthread_mutex_lock(&mutex);
    if( bItem-aItem+1 >= maxItem ) {
      // too many requests. reject
      fprintf(stderr,"Queue full.\n");
      pthread_mutex_unlock(&mutex);
      return MHD_NO;
    }
    pthread_mutex_unlock(&mutex);

    // new queue item
    int i = bItem % maxItem;

    // save connection
    *con_cls = &queue[i]; 
    queue[i].connection = connection;

    queue[i].c1 = atoi(c1);
    queue[i].c2 = c2 ? atoi(c2) : -1;

    const char* d1 = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "d1");
    const char* d2 = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "d2");

    queue[i].d1 = d1 ? atoi(d1) : -1;
    queue[i].d2 = d2 ? atoi(d2) : -1;

    fprintf(stderr,"End of handle connection (1st call). %x (%d,%d)\n", long(connection), aItem, bItem);

    // append to the queue and signal worker thread
    pthread_mutex_lock(&mutex);
    bItem++;
    pthread_cond_signal(&condition);
    pthread_mutex_unlock(&mutex);

    return MHD_YES; 
  }
    
  *con_cls = connection; 

  // send keep-alive response
  int ret;
  struct MHD_Response *response;
  const char *page = "Waiting...\n";
  response = MHD_create_response_from_buffer(strlen (page), (void*)page, MHD_RESPMEM_PERSISTENT);
  ret = MHD_add_response_header(response,"Connection","Keep-Alive");
  
  fprintf(stderr,"Queuing response.\n");
  ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
  MHD_destroy_response(response);

  fprintf(stderr,"End of handle connection. %x (%d,%d) %d\n", long(connection), aItem, bItem, ret);
  return ret;
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
      pthread_mutex_lock(&mutex);
      int i = aItem++ % maxItem;
      pthread_mutex_unlock(&mutex);

      // compute result
      fprintf(stderr, "Starting compute\n");
      sleep(10);
      fprintf(stderr, "Completed compute\n");

      // post result
      int ret;
      struct MHD_Response *response;
      const char *page = "Done...\n";
      response = MHD_create_response_from_buffer(strlen (page), (void*)page, MHD_RESPMEM_PERSISTENT);
      ret = MHD_add_response_header(response,"Connection","close");
      ret = MHD_queue_response(queue[i].connection, MHD_HTTP_OK, response);
      MHD_destroy_response(response);
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
    // start webserver
    struct MHD_Daemon * d;
    
    if (argc != 2) {
      printf("%s PORT\n", argv[0]);
      return 1;
    }
        
    d = MHD_start_daemon(MHD_USE_DEBUG, atoi(argv[1]), NULL, NULL, &handleRequest, NULL, 
        MHD_OPTION_CONNECTION_TIMEOUT, 0, 
        MHD_OPTION_END);
    
    if (d == NULL) return 1;
    fprintf(stderr,"Server ready.\n");
    
    // TODO: enter compute loop here
    pthread_t compute_thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_JOINABLE );
    if (pthread_create(&compute_thread, &attr, computeThread, NULL)) return 1;

    // server loop
    fd_set rs;
    fd_set ws;
    fd_set es;
    int max;
    while (1) {
      FD_ZERO (&rs);
      FD_ZERO (&ws);
      FD_ZERO (&es);
                              
      // try to get file descriptors
      if (MHD_get_fdset(d, &rs, &ws, &es, &max) != MHD_YES) break;
      
      // wait for FDs to get ready
      fprintf(stderr,"In server loop.\n");
      select(max + 1, &rs, &ws, &es, NULL);
      MHD_run(d);
    }

    MHD_stop_daemon(d);
    return 0;
  }

  free(cat);
  free(tree);
  free(mask);
  free(fbuf[0]);
  free(fbuf[1]);
  return 0;
}
