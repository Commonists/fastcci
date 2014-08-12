#include <stdio.h>
#include <stdlib.h>
#if !defined(__APPLE__)
#include <malloc.h>
#endif
#include <string.h>

#include "fastcci.h"

// the graph
int *cat;
tree_type *tree;

// depth buffer
const int maxdepth = 30;
int *cbuf[maxdepth+1];

// mask/unmask buffer
char *mask;
int *unmask;
int nmask;

inline void count(int v, int d) {
  // lock current category
  cbuf[d][v] = 0;

  // Iterate over subcategories
  int c = cat[v], cend = tree[c];
  c += 2;
  while (c < cend) {
    int w = tree[c];
    cbuf[d][v] += cbuf[d-1][w];
    c++;
  }
}

inline int rcount(int v, int d) {
  if (d==0) return 1;

  // lock current category
  mask[v] = 1;

  // remember for unmasking
  unmask[nmask++] = v;

  // Iterate over subcategories
  int count = 0;
  int c = cat[v], cend = tree[c];
  c += 2;
  while (c < cend) {
    int w = tree[c];
    if (mask[w]==0)
      count += rcount(w, d-1);
    c++;
  }
}

int main() {
  int maxcat = readFile("../fastcci.cat", cat);
  maxcat /= sizeof(int);

  mask = (char*)malloc(maxcat);
  memset(mask, 0, maxcat);
  unmask = (int*)malloc(maxcat * sizeof(int));

  for (int i=0; i<=maxdepth; ++i)
  {
    cbuf[i] = (int*)malloc(maxcat * sizeof(*cbuf));
    memset(cbuf[i], 0, maxcat * sizeof(int));
  }

  readFile("../fastcci.tree", tree);

#if 0
  // recursion
  for (int d=1; d<=maxdepth; ++d)
  {
    printf("Analyzing depth %d recursively...\n", d);
    for (int v=0; v<maxcat; ++v)
      if (cat[v]>-1)
      {
        // count
        nmask = 0;
        cbuf[d][v] = rcount(v,d);

        // unmask
        while (nmask--) { mask[unmask[nmask]] = 0; }
      }
  }

#else

  // initialize generate depth buffer 0
  for (int v=0; v<maxcat; ++v) cbuf[0][v] = 1;

  for (int d=1; d<=maxdepth; ++d)
  {
    printf("Analyzing depth %d iteratively...\n", d);
    for (int v=0; v<maxcat; ++v)
      if (cat[v]>-1)
        count(v, d);
  }
#endif

  // generate histograms
  int maxsize[maxdepth+1];
  int maxmax = 0;
  for (int d=0; d<=maxdepth; ++d)
  {
    // find maximum size and mean (median?)
    maxsize[d] = 0;
    int n = 0; 
    double sum = 0.0;
    for (int v=0; v<maxcat; ++v)
      if (cat[v]>-1) 
      {
        if (cbuf[d][v]>maxsize[d])
          maxsize[d] = cbuf[d][v];
        n++;
        sum += cbuf[d][v];
      }
    if (maxsize[d]>maxmax) maxmax = maxsize[d];
    //printf("maxsize[%d] = %d\n", d, maxsize[d]);
    printf("%d %d\n", d, int(sum/double(n)));
  }

  return 0;

  // output histogram
  int *hist = (int*)malloc((maxmax+1) * sizeof(int));

  for (int d=0; d<=maxdepth; ++d)
  {
    memset(hist, 0, (maxsize[d]+1) * sizeof(*hist));
    for (int v=0; v<maxcat; ++v)
      if (cat[v]>-1)
        hist[cbuf[d][v]]++;

    for (int i=0; i<=maxsize[d]; ++i)
      printf("%d %d\n", i, hist[i]);
  }

  free(cat);
  free(tree);

  free(cbuf);

  return 0;
}
