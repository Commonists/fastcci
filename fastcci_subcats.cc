#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

int *readFile(const char *fname) {
  FILE *in = fopen(fname,"rb");
  fseek(in, 0L, SEEK_END);
  int sz = ftell(in);
  fseek(in, 0L, SEEK_SET);
  int *buf = (int*)malloc(sz);
  fread(buf, 1, sz, in);
  return buf;
}

int main(int argc, char *argv[]) {
  if (argc!=2) exit(1);
  int rootcat = atoi(argv[1]);

  int *cat  = readFile("../fastcci.cat");
  int *tree = readFile("../fastcci.tree");

  int i = cat[rootcat], cstart = i+2, cend = tree[i], cfile = tree[i+1];
  printf("Found %d subcats tree[%d]=%d %d %d\n", cend-cstart,i, tree[i], tree[i+1], tree[i+2]);

  for (int j=cend; j<cfile; j++) printf("%d\n",tree[j]);

  free(cat);
  free(tree);
  return 0;
}
