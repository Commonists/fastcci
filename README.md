FastCCI
=======

![FastCCI Logo](http://i.imgur.com/OPQOsVK.png)

Fast Commons Category Inspection is an in-memory database for fast commons category operations such as

* Loop detection
* Deep traversal
* Category intersection
* Category subtraction

FastCCI can operate without depth limits on categories.

```fastcci_build_db``` builds the binary database files form an SQL dump of the categorylinks database.

```fastcci_server``` is the database server backend that can be queried through HTTP.

## Where is FastCCI used?

An instance of the FastCCI backend is running on Wikimedia Labs at [http://fastcci1.wmflabs.org/](http://fastcci1.wmflabs.org/status). A frontend is available on Wikimedia Commons as a gadget ([Click here to install](https://commons.wikimedia.org/w/index.php?title=Help:FastCCI&withJS=MediaWiki:ActivateGadget.js&gadgetname=fastcci)).

## Preparing database

The database is generated from a simple parent child pageid table that is generated with a short SQL query. On Wikimedia Tool Labs this query can be launched with the following command. 
The text output is streamed into the ```fastcci``` command that parses it and generates a binary database image, containing of the ```fastcci.cat``` index file and the ```fastcci.tree``` data file.
Both files are saved to the current directory.

```
mysql --defaults-file=$HOME/replica.my.cnf -h commonswiki.labsdb commonswiki_p -e 'select /* SLOW_OK */ cl_from, page_id, cl_type from categorylinks,page where cl_type!="page" and page_namespace=14 and page_title=cl_to order by page_id;' --quick --batch --silent | ./fastcci_build_db
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
  * ```fqv``` List all FPs, QIs, and VIs files (in that order) in and below category ```c1```
  * ```path``` Find the subcategory path from category ```c1``` to file or category ```c2```


The server performs some sanity checking on the query parameters to make sure that the pageids supplied are pointing to categories (or if allowed to files).

### Response format

The response is delivered in a simple text format with multiple lines. Each line starts with a keyword and may be followed by data. The keywords are:

* ```RESULT``` followed by a ```|``` separated list of  up to 50 integer triplets of the form ```pageId,depth,tag```. Each triplet stands for one image or category.
* ```NOPATH``` indicates that no path from ```c1``` to ```c2``` in a ```a=path``` request was found.
* ```OUTOF``` followed by an integer that is the number of total items in th the calculated result (rather than the number of returned items). This can be either an exact number (for ```a=list```) or an estimate (for ```a=and``` and ```a=not```).
* ```QUEUED``` is the immediate acknowledgement that the server has queued the current request.
* ```WAITING``` is sent to the client with one integer value representing the number of requests that are ahead in the queue and will be processed before the current request.
* ```WORKING``` followed by two integers representing the current number of items found in  ```c1``` and ```c2```. This response item is sent to the client every 0.2s and shows the current state of the ongoing category traversal.
* ```DONE``` indicates the end of the server transmission.

## Command line tools

* ```fastcci_tarjan``` uses [Tarjan's Algorithm](https://en.wikipedia.org/wiki/Tarjan%E2%80%99s_strongly_connected_components_algorithm) to find _strongly coupled components_ in the category graph. Those are essentially conencted clusters of loops.
* ```fastcci_circulartest``` uses a custom algorithm to find individual category loops. Unlike ```fastcci_tarjan``` this also catches self referencing categories. It may however ommit loops that share nodes with other loops. 
* ```fastcci_subcats cat_id``` outputs the direct subcategories of the category specified by ```cat_id``` (this is mostly for debugging)

