#include <stdio.h>
#include <stdlib.h>
#if !defined(__APPLE__)
#include <malloc.h>
#endif

int compare (const void * a, const void * b) {
  return ( *(int*)a - *(int*)b );
}

int main(int argc, char *argv[]) {
	int i;
	if (argc!=2) exit(1);

	// circular category detection
	int maxcat = atoi(argv[1])+1;
	int *cat = (int*)malloc(sizeof(int)*maxcat);
	for (i=0; i<maxcat; ++i)
		cat[i] = -1;

	// category tree buffer
	const int maxtree = 150000000;
	int *tree = (int*)malloc(sizeof(int)*maxtree);
	int cstart=0, cfile=0, csubcat=0;

	// read data dump line by line
	char buf[200], type[10];
	int cl_from, cl_to, lcl_to=-1;
	while(!feof(stdin)) {
		if (fgets(buf, 200, stdin)) {
			sscanf(buf,"%d %d %s", &cl_from, &cl_to, type);

			// new category?
			if (cl_to != lcl_to) {
				if (lcl_to>0) {
					cat[lcl_to] = cstart;
					tree[cstart]   = csubcat;
					tree[cstart+1] = cfile;
                    // pre-sort the file list
                    qsort(&(tree[csubcat]),cfile-csubcat,sizeof(int),compare);
				}
				cstart = csubcat = cfile;
				csubcat += 2;
				cfile   += 2;
			}
			lcl_to = cl_to;

			if (type[0]=='s') {
				tree[csubcat++] = cl_from;
				cfile++;
			} else if (type[0]=='f') {
				tree[cfile++] = cl_from;
			}

			if (cfile==maxtree) {
				printf("Tree buffer too small\n");
				exit(1);
			}
		}
	}
	cat[lcl_to] = cstart;
	tree[cstart]   = csubcat;
	tree[cstart+1] = cfile;
    // pre-sort the file list
    qsort(&(tree[csubcat]),cfile-csubcat,sizeof(int),compare);

	// write out binary tree files
	FILE *outtree = fopen("../fastcci.tree","wb");
	FILE *outcat  = fopen("../fastcci.cat","wb");

	fwrite(tree, sizeof(int), cfile,  outtree);
	fwrite(cat,  sizeof(int), maxcat, outcat);

	fclose(outtree);
	fclose(outcat);

	return 0;
}
