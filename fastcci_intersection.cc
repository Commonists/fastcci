#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
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

const int maxdepth=300;

int resbuf;
int *fbuf[2] = {0}, fmax[2]={100000,100000}, fnum[2];

int *cat; 
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

int main(int argc, char *argv[]) {
  if (argc!=3) exit(1);
  int cid[2] = {atoi(argv[1]), atoi(argv[2])};

  int maxcat = readFile("../fastcci.cat", cat);
  maxcat /= sizeof(int);
  mask = (char*)malloc(maxcat);

  readFile("../fastcci.tree", tree);

  // intermediate return buffers
  fbuf[0]=(int*)malloc(fmax[0]*sizeof(int));
  fbuf[1]=(int*)malloc(fmax[1]*sizeof(int));

  // generate intermediate results
  for (int i=0; i<2; ++i) {
    // clear visitation mask
    memset(mask,0,maxcat);
    
    // fetch files through deep traversal
    resbuf=i;
    fetchFiles(cid[i],0);
    fprintf(stderr,"fnum %d\n", fnum[i]);

    // sort the result buffer
    qsort(fbuf[i], fnum[i], sizeof(int), compare);
  }

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

  free(cat);
  free(tree);
  free(fbuf[0]);
  free(fbuf[1]);
  return 0;
}
