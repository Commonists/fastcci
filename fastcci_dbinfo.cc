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

int main() {
  int maxcat = readFile("../fastcci.cat", cat);
  maxcat /= sizeof(int);

  readFile("../fastcci.tree", tree);

  double filecount = 0, catcount = 0, catrelcount = 0, fileincatcount = 0;

  for (int v=0; v<maxcat; ++v)
  {
    if (cat[v]>-1)
    {
      catcount++;
      int i = cat[v], cstart = i+2, cend = tree[i], cfile = tree[i+1];
      catrelcount += cend-cstart;
      fileincatcount += cfile-cend;
    }
    else
      filecount++;
  }

  printf("%f files\n", filecount);
  printf("%f categories\n", catcount);
  printf("%f 'category is subcategory of' relations\n", catrelcount);
  printf("%f 'file is in category' relations\n", fileincatcount);

  free(cat);
  free(tree);

  return 0;
}
