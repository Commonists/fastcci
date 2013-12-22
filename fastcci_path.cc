/*******************************
 *  fastcci_path cat1 cat2
 *    calculate the subcategory 
 *    path that takes you from 
 *    cat1 to cat 2
 *******************************/

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
int *cat, cat1, cat2; 
int *tree; 
char *mask;
bool found = false;

void tagCat(int id, int depth) {
  // record path
  if (depth==maxdepth || mask[id]!=0 || found) return;
  history[depth] = id;

  // found the target category
  if (id==cat2) {
    printf("Connecting path found!\n");
    for (int i=0; i<=depth; ++i) {
      printf("%d|", history[i]);
    }
    printf("\n");
    found = true;
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
}

int main(int argc, char *argv[]) {
  if (argc!=3) exit(1);
  cat1 = atoi(argv[1]);
  cat2 = atoi(argv[2]);

  int maxcat = readFile("../fastcci.cat", cat);
  maxcat /= sizeof(int);
  mask = (char*)malloc(maxcat);

  readFile("../fastcci.tree", tree);

  memset(mask,0,maxcat);
  tagCat(cat1,0); 

  if (!found) printf("No connection found.\n");

  free(cat);
  free(tree);
  return 0;
}
