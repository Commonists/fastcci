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

## Query syntax

Start the server with ```./fastcci_server PORT```, where ```PORT``` is the tcp port the server will listen. The ```fastcci.cat``` and ```fastcci.tree``` files have to be in the parent directory of the executable.

The server can be queried through HTTP or WebSockets. The URLs are the same in both cases (except for the protocol part). The request string looks like an ordinary HTTP GET URL.
assuming the server was started on port 8080 you can query it using curl like this:

```
curl 'http://localhost:8080/?c1=9986&c2=26398707&a=path'
```

### Query parameters

```c1``` The primary category pageid integer value. This always has to be specified, otherwise the server will return an error 500.
```c2``` The secondary category (or file) pageid
```d1``` The primary search depth (defaults to infinity)
```d2``` The secondary search depth (defaults to infinity)
```a``` The query action. Values can be
  ```and``` Perform the intersection between category ```c1``` and category ```c2``` (default action)
  ```not``` Fetch fils that are in category ```c1``` but not in category ```c2```
  ```list``` List all files in and below category ```c1```
  ```path``` Find the subcategory path from category ```c1``` to file or category ```c2```



