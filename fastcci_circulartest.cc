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
int subcatcount;
int *cat; 
int *tree; 
char *mask;

void tagCat(int id, int depth) {
  // record path
  if (depth==maxdepth) return;
  history[depth] = id;

  // previously visited category
  int i;
  if (mask[id] != 0) {
    // is it a loop (check history)
    for (i=0; i<depth; ++i) {
      if (history[i] == id) break;
    }
    if (i==depth) return; // not a loop
     printf("%d: ",history[0]);
    for (; i<=depth; ++i) {
      printf("%d|", history[i]);
    }
    printf("\n");
    return;
  }

  // mark as visited
  mask[id]=1;
  subcatcount++;
  int c = cat[id], cend = tree[c];
  c += 2;
  while (c<cend) {
    tagCat(tree[c], depth+1);
    c++; 
  }
}

int main() {
  int maxcat = readFile("fastcci.cat", cat);
  maxcat /= sizeof(int);
  mask = (char*)malloc(maxcat);

  readFile("fastcci.tree", tree);

  int maxsubcatcount = 0;
  memset(mask,0,maxcat);
  for (int i=0; i<maxcat; ++i) {
    if (cat[i]>-1 && mask[i]==0) {
      subcatcount = 0;
      tagCat(i,0); 
      /*if (subcatcount > maxsubcatcount) {
      }*/
    }
  }

  free(cat);
  free(tree);
  return 0;
}
