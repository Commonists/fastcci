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
int history[maxdepth];
int *cat; 
int *tree; 
char *mask;

void tagCat(int id, int depth) {
  // previously detected loop category
  if (mask[id] == 2) return;

  // record path
  if (depth==maxdepth) return;
  history[depth] = id;

  // previously visited during this traversal
  if (mask[id] == 1) {
    int i;

    // output and mark entire loop 
    for (i=0; i<=depth && history[i]!=history[depth]; ++i);
    for (; i<=depth; ++i) {
      printf("%d|", history[i]);
      mask[history[i]]=2;
    }

    printf("\n");
    return;
  }

  // mark as visited
  mask[id]=1;
  int c = cat[id], cend = tree[c];
  c += 2;
  while (c<cend) {
    tagCat(tree[c], depth+1);
    c++; 
  }

  // back-track
  if (mask[id]==1) mask[id]=0;
}

int main() {
  int maxcat = readFile("fastcci.cat", cat);
  maxcat /= sizeof(int);
  mask = (char*)malloc(maxcat);

  readFile("fastcci.tree", tree);

  memset(mask,0,maxcat);
  for (int i=0; i<maxcat; ++i) {
    if (cat[i]>-1 && mask[i]==0) {
      tagCat(i,0); 
    }
  }

  free(cat);
  free(tree);
  return 0;
}
