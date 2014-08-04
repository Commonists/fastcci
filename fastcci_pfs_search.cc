#include <stdio.h>
#include <stdlib.h>
#include "fastcci.h"
#if !defined(__APPLE__)
#include <malloc.h>
#endif

// Search categories with a given number of Parent cat, File, and Sub cat counts 

int main(int argc, char *argv[]) {
  if (argc!=4) exit(1);

  int P = atoi(argv[1]);
  int F = atoi(argv[1]);
  int S = atoi(argv[1]);

  int *cat;
  int maxcat = readFile("../fastcci.cat", cat);
  maxcat /= sizeof(int);

  tree_type *tree;
  readFile("../fastcci.tree", tree);

  // parent category counter
  int *pcc = (int*)malloc(maxcat * sizeof(int));
  memset(pcc, 0, maxcat * sizeof(*pcc));

  // go over all cats and increment pcc for the subcats
  printf("Counting each categorie's parents...\n");
  for (int v=0; v<maxcat; ++v)
    if (cat[v]>-1)
    {
      int i = cat[v], cstart = i+2, cend = tree[i];
      // increase parent cat counter
      for (int j=cstart; j<cend; ++j) pcc[tree[j]]++;
    }

  // go over all cats and compare pcc, file count, and subcat count
  printf("Matching...\n");
  int nummatch = 0;
  for (int v=0; v<maxcat; ++v)
    if (cat[v]>-1)
    {
      int i = cat[v], cstart = i+2, cend = tree[i], cfile = tree[i+1];
      // counters
      int nfile = cfile - cend;
      int scc   = cend - cstart;

      if (pcc[v]==P && nfile==F && scc==S) {
        nummatch++;
        printf("%d|", v);
      }
    }

  printf("\n%d matches found.\n", nummatch);

  free(cat);
  free(tree);
  return 0;
}
