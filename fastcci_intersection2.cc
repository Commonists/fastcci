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

// current result buffer
int resbuf;

// sorted and de-duplicated results
int *fbuf[2] = {0};
// list of all subcategory file sets to be merged
int **kbuf[2] = {0}, kmax[2]={1024*1024,1024*1024}, knum[2];

int *cat; 
int *tree; 
char *mask;

// recursively traverse the graph an accumulate subcategories
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

  // if this cat contains files add it to the results list
  if (c<cfile) {
    // grow buffer on demand
    if (knum[resbuf]+2 >= kmax[resbuf]) {
      kmax[resbuf] *= 2;
      kbuf[resbuf] = (int**)realloc(kbuf[resbuf], kmax[resbuf]*sizeof(int*));
    }

    // copy file list pointers
    kbuf[resbuf][knum[resbuf]]   = &(tree[c]);
    kbuf[resbuf][knum[resbuf]+1] = &(tree[cfile]);
    knum[resbuf] += 2;
  }
}

// comparator for bsearch (TODO: implement own bsearch. Should be faster without the extra function call to the comparator)
int compare (const void * a, const void * b) {
  return ( *(int*)a - *(int*)b );
}

// min heap
#define CMP(a, b) ((a) <= (b))

// the heap. we grow this on demand and keep the memory allocated.
int *mheap=NULL, *heap, nheap, maxheap=0;


inline void heapPush(int k, int *heap) {
  int i=++nheap, val = *(kbuf[resbuf][k]);
  for(; i > 1 && CMP(heap[i>>1], val); i = i>>1 ) heap[i] = heap[i>>2];
  heap[i] = k;
}

void heapPop() {
  int i=0;
  while(true) {
    int l=(i<<1)+1, r= (i<<1)>+2;
    if (l>=nheap||r>=nheap) break;
  }
}

// generate a sorted and deduplicated intermediate result set
int heapMerge() {
  // number of sorted lists to merge
  int k = knum[resbuf]/2;

  // reserve heap
  if (k>maxheap) {
    maxheap=k;
    mheap = (int*)realloc(mheap,k*sizeof(int));
    heap = mheap-1;
  }
  
  // initial heap population (each heap item is the number of a subcategory file list)
  nheap=0;
  for (int i=0; i<knum[resbuf]; i+=2 ) heapPush(i,heap);

  int r, lr=-1,a, val;
  while (nheap>0) {
    // fetch the next item from the list at the top of the heap
    r = *(kbuf[resbuf][heap[1]]++);

    // append to output if different from previous value
    if (r!=lr) {
    }

    // if the list in the heap root has elements left leave it in the heap otherwise
    if (kbuf[resbuf][heap[1]]==kbuf[resbuf][heap[1]+1]) {
      // remove it (put the last item on the heap in its place)
      heap[1] = heap[nheap--];
    }

    // percolate the current heap root down
    val = *(kbuf[resbuf][heap[1]]);
  }
}

int main(int argc, char *argv[]) {
  if (argc!=3) exit(1);
  int cid[2] = {atoi(argv[1]), atoi(argv[2])};

  // load the category index with pointers into the tree object
  int maxcat = readFile("../fastcci.cat", cat);
  maxcat /= sizeof(int);
  mask = (char*)malloc(maxcat);

  // load the raw subcat/file relation data
  readFile("../fastcci.tree", tree);

  // intermediate return buffers
  kbuf[0]=(int**)malloc(kmax[0]*sizeof(int*));
  kbuf[1]=(int**)malloc(kmax[1]*sizeof(int*));

  // generate intermediate results
  for (int i=0; i<2; ++i) {
    // clear visitation mask
    memset(mask,0,maxcat);
    
    // fetch files through deep traversal
    resbuf=i;
    fetchFiles(cid[i],0);
    fprintf(stderr,"%d subcategories included.\n", knum[i]/2);
  }

  exit(0); // break for now

#if 0
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

    // heap merge the result set

    //qsort(fbuf[small], fnum[small], sizeof(int), compare);

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
#endif 

  free(cat);
  free(tree);
  free(kbuf[0]);
  free(kbuf[1]);
  free(heap);
  return 0;
}
