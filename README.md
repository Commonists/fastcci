fastcci
=======

Fast Commons Category Inspection is an in-memory database for fast commons category operations such as

* Loop detection
* Deep traversal
* Category intersection

fastcci can operate without depth limits on categories.

```fastcci``` builds the binary database files form an SQL dump of the categorylinks database.

```fastcci_server``` is the database server backend that can be queried through HTTP.

## Preparing database

```
mysql --defaults-file=$HOME/replica.my.cnf -h commonswiki.labsdb commonswiki_p -e 'select /* SLOW_OK */ cl_from, page_id, cl_type from categorylinks,page where cl_type!="page" and page_namespace=14 and page_title=cl_to order by page_id;' --quick --batch --silent > $HOME/commons_categories.txt
```

then call 

```
./fastcci maxcat < $HOME/commons_categories.txt
```

where ```maxcat``` is the highest category number (this will be made automatic).
