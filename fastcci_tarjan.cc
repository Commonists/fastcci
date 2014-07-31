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

// the graph
int *cat; 
int *tree; 

// Tarjan's id and lowlink fields
int *id;
int *lowlink;

// current id value
int index = 1;

// the stack
int s = 0;
int *S;
char *Smask;

void strongconnect(int v) {
  int w;

  // Set the depth index for v to the smallest unused index
  id[v] = lowlink[v] = index++;

  // push to the stack
  S[s++] = v;
  Smask[v]++;

  // Consider successors of v
  int c = cat[id], cend = tree[c];
  c += 2;
  while (c<cend) {
    w = tree[c];
    if (id[w] == 0) 
    {
      // Successor w has not yet been visited; recurse on it
      strongconnect(w)
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
  if (lowlink[v] = id[v])
  {
    // start a new strongly connected component
    do {
      // pop from stack
      w = S[--s]; 
      Smask[w]--;

      //add w to current strongly connected component
      printf("%d ", w);
    } while  (w != v)
    printf("\n");
  }
}

int main() {
  int maxcat = readFile("../fastcci.cat", cat);
  maxcat /= sizeof(int);
  
  id      = (int*)malloc(maxcat);
  lowlink = (int*)malloc(maxcat);
  memset(id, 0, maxcat * sizeof(*id));

  S = (int*)malloc(maxcat);
  Smask = (char*)malloc(maxcat);
  memset(Smask, 0, maxcat);

  readFile("../fastcci.tree", tree);

  for (int v=0; v<maxcat; ++v)
    if (cat[v]>-1 && id[v]==0) 
      strongconnect(i); 

  free(cat);
  free(tree);

  free(id);
  free(lowlink);
  free(S);
  free(Smask);

  return 0;
}
