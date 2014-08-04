#include <stdio.h>
#include <stdlib.h>
#include "fastcci.h"
#if !defined(__APPLE__)
#include <malloc.h>
#endif

/**
 * Find all small category diamonds:
 *
 *     A
 *    / \
 *   B   C
 *    \ /
 *     D
 */

int main(int argc, char *argv[]) {
  int *cat;
  int maxcat = readFile("../fastcci.cat", cat);
  maxcat /= sizeof(int);

  tree_type *tree;
  readFile("../fastcci.tree", tree);

  // subsubcat masks
  int *ssm1 = (int*)malloc(maxcat * sizeof(int));
  int *ssm2 = (int*)malloc(maxcat * sizeof(int));
  memset(ssm1, 255, maxcat * sizeof(*ssm1));

  // go over all categories that could be the root A of the diamond
  int nummatch = 0;
  for (int v=0; v<maxcat; ++v)
    if (cat[v]>-1)
    {
      int i = cat[v], cstart = i+2, cend = tree[i];
      // go over sub cats and tag sub sub cats
      for (int w=cstart; w<cend; ++w) 
      {
        int j = cat[tree[w]], scstart = j+2, scend = tree[j];

        // go over sub sub cats and tag
        for (int x=scstart; x<scend; ++x) 
        {
          if (ssm1[tree[x]] == v)
          {
            // we've already seen this subsubcat from another subcat
            printf("%d|%d|%d|%d\n", v, ssm2[tree[x]], tree[w], tree[x]); 
            nummatch++;
          }
          else  
          {
            ssm1[tree[x]] = v;
            ssm2[tree[x]] = tree[w];
          }
        }
      }
    }

  printf("\n%d diamonds found.\n", nummatch);

  free(cat);
  free(tree);
  return 0;
}
