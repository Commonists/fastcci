FastCCI
=======

![FastCCI Logo](http://i.imgur.com/OPQOsVK.png)

Fast Commons Category Inspection is an in-memory database for fast commons category operations such as

* Loop detection
* Deep traversal
* Category intersection
* Category subtraction

FastCCI can operate without depth limits on categories.

```fastcci``` builds the binary database files form an SQL dump of the categorylinks database.

```fastcci_server``` is the database server backend that can be queried through HTTP.

## Where is FastCCI used?

An instance of the FastCCI backend is running on Wikimedia Labs at [http://fastcci1.wmflabs.org/](http://fastcci1.wmflabs.org/status). A frontend is available on Wikimedia Commons as a gadget ([Click here to install](https://commons.wikimedia.org/w/index.php?title=Help:FastCCI&withJS=MediaWiki:ActivateGadget.js&gadgetname=fastcci)).

## Preparing database

The database is generated from a simple parent child pageid table that is generated with a short SQL query. On Wikimedia Tool Labs this query can be launched with the following command. 
The text output is streamed into the ```fastcci``` command that parses it and generates a binary database image, containing of the ```fastcci.cat``` index file and the ```fastcci.tree``` data file.
Both files are saved to the current directory.

```
mysql --defaults-file=$HOME/replica.my.cnf -h commonswiki.labsdb commonswiki_p -e 'select /* SLOW_OK */ cl_from, page_id, cl_type from categorylinks,page where cl_type!="page" and page_namespace=14 and page_title=cl_to order by page_id;' --quick --batch --silent | ./fastcci
```

## Query syntax

Start the server with ```./fastcci_server PORT DATADIR```, where ```PORT``` is the tcp port the server will listen, and ```DATADIR``` is the path to the ```fastcci.cat``` and ```fastcci.tree``` files.

The server can be queried through HTTP or WebSockets. The URLs are the same in both cases (except for the protocol part). The request string looks like an ordinary HTTP GET URL.
assuming the server was started on port 8080 you can query it using curl like this:

```
curl 'http://localhost:8080/?c1=9986&c2=26398707&a=path'
```

For backwards compatibility with browsers that do not properly support cross-domain requests a JavaScript callback mode exists, that wraps the result data in a function call to a ```fastcci_callback``` function. This mode is activated by adding the 
```t=js``` query parameter and value.

### Query parameters

* ```c1``` The primary category pageid integer value. This always has to be specified, otherwise the server will return an error 500.
* ```c2``` The secondary category (or file) pageid
* ```d1``` The primary search depth (defaults to infinity)
* ```d2``` The secondary search depth (defaults to infinity)
* ```a``` The query action. Values can be:
  * ```and``` Perform the intersection between category ```c1``` and category ```c2``` (default action)
  * ```not``` Fetch fils that are in category ```c1``` but not in category ```c2```
  * ```list``` List all files in and below category ```c1```
  * ```path``` Find the subcategory path from category ```c1``` to file or category ```c2```


The server performs some sanity checking on the query parameters to make sure that the pageids supplied are pointing to categories (or if allowed to files).

