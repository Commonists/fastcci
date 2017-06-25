#!/bin/bash

# use /tmp because it is on a local filesystem
cd /tmp

query="select /* SLOW_OK */ cl_from, page_id, cl_type from categorylinks,page where cl_type!=\"page\" and page_namespace=14 and page_title=cl_to order by page_id;"
mysql --defaults-file=$HOME/replica.my.cnf -h commonswiki.labsdb commonswiki_p -e "$query" --quick --batch --silent | fastcci_build_db

for server in 2
do
  # try three times to copy the files (in case the key is not available right after a puppet run) 
  for tries in `seq 3`
  do 
    rsync -a fastcci.tree fastcci.cat fastcci-worker${server}:/tmp && break
    sleep 30
  done

  for tries in `seq 3`
  do 
    rsync -a 'done' fastcci-worker${server}:/tmp && break
    sleep 30
  done

  # sleep two minutes to have staggered updates in the cluster
  sleep 120
done
