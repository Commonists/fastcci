#include "fastcci.h"

int maxtree = 150000000, maxcat = 40000000;
tree_type *cat, *tree; 

void growTree(int max=0) {
  if (tree==NULL) 
    tree = (tree_type*)malloc(maxtree * sizeof *tree);
  else {
    maxtree += 10000000;
    if (maxtree<=max) maxtree = max+1;
    tree = (tree_type*)realloc(tree, maxtree * sizeof *tree);
  }
}

void growCat(int max=0) {
  int a;
  if (cat==NULL) {
    cat = (tree_type*)malloc(maxcat * sizeof *cat);
    a = 0;
  } else {
    a = maxcat;
    maxcat += 1000000;
    if (maxcat<=max) maxcat = max+1;
    cat = (tree_type*)realloc(cat, maxcat * sizeof *cat);
  }

  // initialize al cat entries to -1 (unused pageids are files)
  for (int i=a; i<maxcat; ++i) cat[i] = -1;
}


int main(int argc, char *argv[]) {
  int i, j;

  // category index
  growCat();

  // category tree buffer
  growTree();
  int cstart=0, cfile=0, csubcat=0;

  // read data dump line by line
  char buf[200], type[10];
  int cl_from, cl_to, lcl_to=-1;
  int expect; // 0:sf, 1:f
  while(!feof(stdin)) {
    if (fgets(buf, 200, stdin)) {
      sscanf(buf,"%d %d %s", &cl_from, &cl_to, type);

      // new category?
      if (cl_to != lcl_to) {
        if (lcl_to>0) {
          // make sure we have enough memory for the index
          if (lcl_to>=maxcat) growCat(lcl_to);

          // write category index and category header
          cat[lcl_to] = cstart;
          tree[cstart]   = csubcat;
          tree[cstart+1] = cfile;

          // pre-sort the file list
          qsort(&(tree[csubcat]),cfile-csubcat,sizeof *tree,compare);

          // expect a subcategory or a file
        }
        cstart = csubcat = cfile;
        csubcat += 2;
        cfile   += 2;
        expect = 0;
      }

      if (type[0]=='s') {
        if (expect!=0) {
          fprintf(stderr, "Did not expect a subcategory at this point!\n%s\n",buf);
          exit(1);
        }
        if (csubcat>=maxtree) growTree();
        tree[csubcat++] = cl_from;
        cfile++;
      } else if (type[0]=='f') {
        if (cfile>=maxtree) growTree();
        tree[cfile++] = cl_from;
        expect = 1;
      }

      lcl_to = cl_to;
    }
  }

  // make sure we have enough memory for the index
  if (lcl_to>=maxcat) growCat(lcl_to);

  // close final category header
  cat[lcl_to] = cstart;
  tree[cstart]   = csubcat;
  tree[cstart+1] = cfile;
  qsort(&(tree[csubcat]),cfile-csubcat,sizeof(int),compare);

  // verify data
  for (int i=0; i<=lcl_to; ++i) {
    cstart = cat[i];
    if (cstart<0) continue;

    csubcat = tree[cstart];
    cfile = tree[cstart+1];

    cstart += 2;

    // verify subcats
    for(; cstart<csubcat; cstart++) 
      if (tree[cstart] < 0 ) {
        fprintf(stderr,"File in subcat block of cat %d\n", i);
        exit(1);
      }

    // verify files
    for(; cstart<cfile; cstart++) 
      if (tree[cstart] != -1 ) {
        fprintf(stderr,"Category in file block of cat %d\n", i);
        exit(1);
      }
  }

  // write out binary tree files
  FILE *outtree = fopen("../fastcci.tree","wb");
  FILE *outcat  = fopen("../fastcci.cat","wb");

  fwrite(tree, sizeof *tree, cfile,    outtree);
  fwrite(cat,  sizeof *cat,  lcl_to+1, outcat);

  fclose(outtree);
  fclose(outcat);

  free(tree);
  free(cat);
  return 0;
}
