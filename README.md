KRANKER httpd
-------------

KRANKER is a somewhat 'sick' httpd for static content :-)

The socket code is stolen from microhttpd.
There are however, things which KRANKER does very
differently than common webservers:

- On server startup, all documents are read into
  memory. This means that after startup, KRANKER
  will not produce a single disk IO operation.
  This also means that you should have your
  system memory as large as the size of the
  complete document root.

- On server startup, all HTTP Response headers
  are precompiled and stored in memory. On
  doucment delivery, the precompiled headers
  are sent to the client. By doing so, KRANKER
  saves the stat() calls on the files and all
  memory ops involved in creating a response header.

- On server startup, all valid GET requests
  are put into a hashtable. When the server
  receives a request, it will build a hash
  of the first line of the request and
  do a lookup in the hashtable - which again
  means: Zero disk I/O with each request.

- KRANKER supports a 'webtarball' mode. Given
  the path to a tarfile, the contents of the tarfile
  will be used as document root. Would be boring,
  but as tarballs can be 'opened' from any
  http:// URL, this can be fun :-)

- KRANKER can be started with a docroot that contains
  a file upload form. Uploaded files will be stored in
  (and be delivered from) memory, too.

Currently, KRANKER is at the stage of proof-of-concept.
Being there, it delivers static content about 3 times
as fast as squid and apache :-)

Naturally it lacks lots of features:

- No CGIs - only static content (maybe fcgi one day ? :-)
- No POST requests
- No HTTP pipelining (yet)
- Server restart required when content changes
- And much more

- A future version should be able to run without any
  local content, i.e. curl some kind of website tarball :-)

--
[1] microhttpd: http://cygwin.com/ml/cygwin/2001-04/msg01830/uhttpd.c
[2] hash algo: http://burtleburtle.net/bob/c/lookup3.c
