#include <stdio.h>
#include <stdlib.h>
#if !defined(__APPLE__)
#include <malloc.h>
#endif

#include "fastcci.h"

int main(int argc, char *argv[]) {
  if (argc!=2) exit(1);
  int rootcat = atoi(argv[1]);

  int *cat;
  readFile("../fastcci.cat", cat);
  int *tree;
  readFile("../fastcci.tree", tree);

  int i = cat[rootcat], cstart = i+CBEGIN, cend = tree[i], cfile = tree[i+1];
  printf("Found %d subcats tree[%d]=%d %d %d\n", cend-cstart,i, tree[i], tree[i+1], tree[i+2]);

  for (int j=cend; j<cfile; j++) printf("%d\n",tree[j]);

  free(cat);
  free(tree);
  return 0;
}
