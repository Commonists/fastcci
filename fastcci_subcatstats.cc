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
int *dbuf;

int depth(int v) {
  int w, below = 0, belowsub;

  // lock current category
  dbuf[v] = -2;

  // Iterate over subcategories
  int c = cat[v], cend = tree[c];
  c += 2;
  while (c < cend) {
    w = tree[c];

    if (dbuf[w] >= 0)
    {
      // Successor w has been visited (use the previously computed value)
      belowsub = 1 + dbuf[w];
    }
    else if (dbuf[w] == -2)
    {
      // loop (hitting the current parent path of locked categories)
      belowsub = 0;
    }
    else
    {
      // Successor w has not yet been visited; recurse on it
      belowsub = 1 + depth(w);
    } 

    // remember deepest branch
    if (belowsub > below) 
      below = belowsub;

    c++; 
  }

  dbuf[v] = below;
  return below;
}

int main() {
  int maxcat = readFile("../fastcci.cat", cat);
  maxcat /= sizeof(int);
  
  dbuf      = (int*)malloc(maxcat * sizeof(*dbuf));
  memset(dbuf, 255, maxcat * sizeof(*dbuf));

  readFile("../fastcci.tree", tree);

  // generate depth buffer
  for (int v=0; v<maxcat; ++v)
    if (cat[v]>-1 && dbuf[v]==-1) 
      depth(v); 

  // find maximum depth
  int maxdepth = 0;
  for (int v=0; v<maxcat; ++v)
    if (cat[v]>-1 && dbuf[v]>maxdepth) 
      maxdepth = dbuf[v];

  printf("maxdepth = %d\n", maxdepth);

  // output histogram
  int *hist = (int*)malloc((maxdepth+1) * sizeof(int));
  memset(hist, 0, (maxdepth+1) * sizeof(*hist));
  for (int v=0; v<maxcat; ++v)
    if (cat[v]>-1)
      hist[dbuf[v]]++;
  for (int i=0; i<=maxdepth; ++i)
    printf("%d %d\n", i, hist[i]);

  // output maximum path(s)
  for (int v=0; v<maxcat; ++v)
    if (cat[v]>-1 && dbuf[v]==maxdepth)
    {
      int needdepth = maxdepth, w = v;
      printf("%d",v);

      while(needdepth>0)
      {
        needdepth--;
        
        // Iterate over subcategories
        bool match = false;
        int c = cat[w], cend = tree[c];
        c += 2;
        while (c<cend) {
          if (dbuf[tree[c]] == needdepth) {
            w = tree[c];
            printf("|%d", w);
            match = true;
            break;
          }
          c++;
        }
        if (!match) printf("|x");
      }
      printf("\n");
    }

  free(cat);
  free(tree);

  free(dbuf);

  return 0;
}
