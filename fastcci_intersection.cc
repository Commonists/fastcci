#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#if !defined(__APPLE__)
#include <malloc.h>
#endif
#include <string.h>

int readFile(const char *fname, int* &buf) {
  FILE *in = fopen(fname,"rb");
  fseek(in, 0L, SEEK_END);
  int sz = ftell(in);
  fseek(in, 0L, SEEK_SET);
  buf = (int*)malloc(sz);
  fread(buf, 1, sz, in);
  return sz;
}

const int maxdepth=500;

int resbuf;
int *fbuf[2] = {0}, fmax[2]={100000,100000}, fnum[2];

int *cat, maxcat; 
int *tree; 
char *mask;

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

int handleRequest(void *cls, struct MHD_Connection *connection, 
                  const char *url, 
                  const char *method, const char *version, 
                  const char *upload_data, 
                  size_t *upload_data_size, void **con_cls)
{
  // this routine is called by a single thread only (MHD_USE_SELECT_INTERNALLY)

  // still room on the queue?
  if( bItem-aItem+1 >= maxItem ) {
    // too many requests. reject
    return MHD_NO;
  }

  int i = (bItem++) % maxItem;

  // save connection
  queue[i].connection = connection;

  // parse parameters
  const char* c1 = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "c1");
  const char* c2 = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "c2");

  if (c1==NULL) {
    // must supply c1 parameter!
    return MHD_NO;
  }

  queue[i].c1 = atoi(c1);
  queue[i].c2 = c2 ? atoi(c2) : -1;

  const char* d1 = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "d1");
  const char* d2 = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "d2");

  queue[i].d1 = d1 ? atoi(d1) : -1;
  queue[i].d2 = d2 ? atoi(d2) : -1;

  // nudge the compute thread to start processing the queue (if it is not already busy)
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
        
    d = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, atoi(argv[1]), NULL, NULL, &handleRequest, NULL, MHD_OPTION_END);
    
    if (d == NULL) return 1;
    
    // TODO: enter compute loop here
    getc(stdin);

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
