#!/bin/bash

# use /tmp because it is on a local filesystem
cd /tmp
rm -f done

echo START `date` >> $HOME/update_database.log

. $HOME/venv/bin/activate
$HOME/bin/stream_database.py --defaults-file "$HOME/replica.my.cnf" --host commonswiki.analytics.db.svc.eqiad.wmflabs --db commonswiki_p --batch-size 100000 --fetch-size 10000 ||
{
  echo FAILED `date` >> $HOME/update_database.log
  exit
}

for server in 1 2 
do
  # try three times to copy the files (in case the key is not available right after a puppet run) 
  for tries in `seq 3`
  do 
    rsync -a fastcci.tree fastcci.cat fastcci-worker${server}:/tmp && rsync -a 'done' fastcci-worker${server}:/tmp && break
    sleep 30
  done

  # sleep two minutes to have staggered updates in the cluster
  sleep 120
done

echo SUCCESS `date` >> $HOME/update_database.log
