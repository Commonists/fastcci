#include "fastcci.h"
#include "errno.h"

int fd_cat, fd_tree;
int maxtree = 1024*128, maxcat = 1024*128;
tree_type *cat=NULL, *tree=NULL;

void growTree(int max=0) {
  if (tree == NULL) {
    if (ftruncate(fd_tree, maxtree * sizeof *tree))
      printf("fruncate errno=%d fd=%d\n", errno, fd_tree);

    tree = (tree_type*)mmap(NULL, maxtree * sizeof *tree, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_NORESERVE, fd_tree, 0);
  } else {
    int old_maxtree = maxtree;
    maxtree *= 2;
    if (maxtree<=max) maxtree = max+1;
    
    if (ftruncate(fd_tree, maxtree * sizeof *tree))
      printf("errno = %d\n", errno);

    tree = (tree_type*)mremap(tree, old_maxtree * sizeof *tree,  maxtree * sizeof *tree, MREMAP_MAYMOVE);
  }
  
  // check for allocation error
  if (tree == MAP_FAILED) {
    perror("growTree() MAP_FAILED");
    exit(1);
  }
  if (tree == NULL) {
    perror("growTree()");
    exit(1);
  }

  // printf("grew tree to %d\n", maxtree);
}

void growCat(int max=0) {
  int a = 0;

  if (cat == NULL) {
    if (ftruncate(fd_cat, maxcat * sizeof *cat))
      printf("fruncate errno=%d fd=%d\n", errno, fd_cat);

    cat = (tree_type*)mmap(NULL, maxcat * sizeof *cat, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_NORESERVE, fd_cat, 0);
  } else {
    a = maxcat;
    maxcat *= 2;
    if (maxcat<=max) maxcat = max+1;
    ftruncate(fd_cat, maxcat * sizeof *cat);
    cat = (tree_type*)mremap(cat, a * sizeof *cat,  maxcat * sizeof *cat, MREMAP_MAYMOVE);
  }

  // check for allocation error
  if (cat == MAP_FAILED) {
    perror("growCat() MAP_FAILED");
    exit(1);
  }
  if (cat == NULL) {
    perror("growCat() NULL");
    exit(1);
  }

  // initialize al cat entries to -1 (unused pageids are files)
  for (int i=a; i<maxcat; ++i) cat[i] = -1;

  // printf("grew cat to %d\n", maxcat);
}


int main(int argc, char *argv[]) {
  int i, j;

  // file descriptors for mmap
  fd_cat = open("fastcci.cat", O_RDWR|O_CREAT, 0744);
  if (fd_cat == -1) {
    printf("Error opening cat file: %d\n", errno);
    return 1;
  }
  fd_tree = open("fastcci.tree", O_RDWR|O_CREAT, 0744);
  if (fd_tree == -1) {
    printf("Error opening tree file: %d\n", errno);
    return errno;
  }

  growCat();
  growTree();

  // insert empty dummy category at tree[0]
  int cstart=2, cfile=2, csubcat=2;
  tree[0] = 2;
  tree[1] = 2;

  // read data dump line by line
  char buf[200], type[10];
  int cl_from, cl_to, lcl_to=-1;
  int expect; // 0:sf, 1:f
  while (!feof(stdin)) {
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
      }

      if (cfile>=maxtree) growTree();

      if (type[0]=='s') {
        // is the cat index of this subcategory still -1, then set it to the empty dummy cat 0
        if (cl_from>=maxcat) growCat(cl_from);
        if (cat[cl_from]==-1) cat[cl_from]=0;

        // no files in category yet?
        if (csubcat==cfile) {
          tree[csubcat++] = cl_from;
          cfile++;
        } else {
          // we already have a file at tree[subcat], move it to the end of cfile
          //fprintf(stderr,"out of order subcat!\n");
          tree[cfile++] = tree[csubcat];
          tree[csubcat++] = cl_from;
        }
      } else if (type[0]=='f') {
        // just append at cfile
        tree[cfile++] = cl_from;
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

    if (csubcat<cstart) {
      fprintf(stderr,"Negative subcat block length in cat %d\n", i);
      exit(1);
    }
    if (cfile<csubcat) {
      fprintf(stderr,"Negative file block length in cat %d\n", i);
      exit(1);
    }

    // verify subcats
    for (; cstart<csubcat; cstart++)
      if (cat[tree[cstart]] < 0 ) {
        fprintf(stderr,"File %d in subcat block of cat %d\n", tree[cstart], i);
        exit(1);
      }

    // verify files
    for (; cstart<cfile; cstart++)
      if (cat[tree[cstart]] != -1 ) {
        fprintf(stderr,"Category %d in file block of cat %d\n", tree[cstart], i);
        exit(1);
      }
  }

  // write out binary tree files
  munmap(cat, maxcat * sizeof *cat);
  munmap(tree, maxtree * sizeof *tree);
  ftruncate(fd_tree, cfile * sizeof *tree);
  ftruncate(fd_cat, (lcl_to+1) * sizeof *cat);
  close(fd_tree);
  close(fd_cat);

  printf("db files written.\n");

  // write success file
  FILE *done = fopen("done", "w");
  if (done)
  {
    fprintf(done, "OK\n");
    fclose(done);
  }
  else
    printf("Could not open file.\n");

  return 0;
}
