#!/bin/bash
cd $HOME/data

query="select /* SLOW_OK */ cl_from, page_id, cl_type from categorylinks,page where cl_type!=\"page\" and page_namespace=14 and page_title=cl_to order by page_id;"
mysql --defaults-file=$HOME/replica.my.cnf -h commonswiki.labsdb commonswiki_p -e "$query" --quick --batch --silent | fastcci_build_db
