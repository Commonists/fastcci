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

// Tarjan's id and lowlink fields
int *id;
int *lowlink;

// current id value
int curr_id = 1;

// the stack
int s = 0;
int *S;
char *Smask;

int *scc;

void strongconnect(int v) {
  int w;

  // Set the depth index for v to the smallest unused index
  id[v] = lowlink[v] = curr_id++;

  // push to the stack
  S[s++] = v;
  Smask[v]++;

  // Consider successors of v
  int c = cat[v], cend = tree[c];
  c += CBEGIN;
  while (c<cend) {
    w = tree[c];
    if (id[w] == 0)
    {
      // Successor w has not yet been visited; recurse on it
      strongconnect(w);
      lowlink[v] = lowlink[w] < lowlink[v] ? lowlink[w] : lowlink[v];
    }
    else if (Smask[w])
    {
      // Successor w is in stack S and hence in the current SCC
      lowlink[v] = id[w] < lowlink[v] ? id[w] : lowlink[v];
    }

    c++;
  }

  // If v is a root node, pop the stack and generate an SCC
  if (lowlink[v] == id[v])
  {
    // start a new strongly connected component
    int n = 0;
    do {
      // pop from stack
      w = S[--s];
      Smask[w]--;

      //add w to current strongly connected component
      scc[n++] = w;
    } while  (w != v);

    // do not print single node SCCs
    if (n>1) {
      printf("%d : ", n);
      for (int i=0; i<n; ++i)
        printf(i>0 ? "|%d" : "%d", scc[i]);
      printf("\n");
    }
  }
}

int main() {
  int maxcat = readFile("../fastcci.cat", cat);
  maxcat /= sizeof(int);

  lowlink = (int*)malloc(maxcat * sizeof(*lowlink));
  id      = (int*)malloc(maxcat * sizeof(*id));
  memset(id, 0, maxcat * sizeof(*id));

  S = (int*)malloc(maxcat * sizeof(*S));
  Smask = (char*)malloc(maxcat);
  memset(Smask, 0, maxcat);

  // scc result buffer
  scc = (int*)malloc(maxcat * sizeof(*scc));

  readFile("../fastcci.tree", tree);

  for (int v=0; v<maxcat; ++v)
    if (cat[v]>-1 && id[v]==0)
      strongconnect(v);

  free(cat);
  free(tree);

  free(id);
  free(lowlink);
  free(S);
  free(Smask);

  return 0;
}
