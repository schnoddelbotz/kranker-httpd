/*
 * KRANKER httpd
 * a somewhat sick :-) httpd.
 * on startup, KRANKER slurps the whole document root into
 * memory. while doing so, KRANKER creates a GET hash lookup
 * table and precompiles HTTP repsonse headers.
 * Once started, KRANKER will produce zero disk I/O (except
 * docroot doesnt fit into memory and swapping takes place).
 * KRANKER can be for you:
 * - if you have as dobule as mouch memory as the total
 *   size of your website (try 'du -sk $homepage_dir')
 * - if you only use static content - no CGI.
 * TODO:
 * - use inotify for local docroot updates? :-)
 * - sane memory / docrootsize check
 * - make lynx happy (+content type!)
 * READ ----> http://pl.atyp.us/content/tech/servers.html
 * - ...
 * - logging: unformatted (binlog) to udp loghost
 * hash.c is a copy of
 * http://burtleburtle.net/bob/c/lookup3.c
 * the webserver is founded on
 * http://cygwin.com/ml/cygwin/2001-04/msg01830/uhttpd.c
 * TODO:
 * -t TAR	use tarfile as docroot, can be from http://...tar
 * -w		writeable docroot, enable HTTP PUT (provide upload!)
 * add option+signalhandler+web-if to dump memory to local directory
 * ... kranker-"cluster": push content between nodes as tar
 * ... add opt to fwd all non-200 requests via Location: header?
 * ...				allow load-balancing over multi locations
 * ... post status to given url on successful startup?
 * make it an osx drag-n-drop .app
*/
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <sys/socket.h> /* for socket(), bind(), and connect() */
#include <arpa/inet.h>  /* for sockaddr_in and inet_ntoa() */
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>

#define MAXFILES 32768
#define MAXTHREADS 8
#define MAXPENDING 5    /* Maximum outstanding connection requests */
#define DEFAULT_PORT 8080

#define DOC_IS_NOINDEX   0
#define DOC_IS_INDEX	 1

#define DOCMODE_MEMORY   1	/* static document in memory */
#define DOCMODE_FUNCTION 2	/* function provides content */
#define DOCMODE_FASTCGI  3	/* fwd to fastcgi sock */
#define DOCMODE_FILE     4	/* pointer to external file (eeeek) */

int read_docroot(char *docdir);
int add_document(char* root, char* filename, char* dirname, char* filebasename, struct stat s, int isindex, int docmode);
int scan_docs(char* root, char* dir);
int valid_content(char* filename);
int CreateTCPServerSocket(unsigned short port);
void DieWithError(char *errorMessage);  /* Error handling function */
void HandleSignal(int sig, siginfo_t *si, void *context);
//extern uint32_t hashlittle( const void *key, size_t length, uint32_t initval);
extern unsigned long realpath_cache_key(const char *path, int path_len);
//char *tsrm_strtok_r(char *s, const char *delim, char **last);
extern int kranker_open_tar(char* userpath);

//int read_docroot(char* docdir);
char *baseurl="/";
int  doccount = 0;
int  verbose = 0;
int  filecount = 0;
int  runasdaemon  = 0;
int  srvport = DEFAULT_PORT;
int  maxrequests = 99999999;
char* kversion = "0.0.1b";
size_t  docrootbytes, sentbytes; // FIXME add with each file, spit out when going online...
char *docs[MAXFILES];
//struct docinfo { uint32_t hash; int docid; char* url; size_t filesize; char* header; size_t headersize; int doctype; };
struct docinfo { unsigned long hash; int docid; char* url; size_t filesize; char* header; size_t headersize; int docmode; };
struct docinfo docinfo[MAXFILES];
int hashtab[MAXFILES];
unsigned long conno = 0; // connection counter
char* indexdocp;

static char usage_msg[] =
"KRANKER is a somewhat sick httpd.\n\
 usage:\n\
    kranker [...options...]\n\
 	 -D 		run as daemon\n\
 	 -d DOCROOT	use given dir as document root\n\
	 -t TAR/URL	read [http://...].tar.* file and use it as docroot\n\
 	 -v		increase verbosity\n\
 	 -i FILE	use FILE as directory index\n\
 	 -p PORT	run on port PORT\n\
	 -r NUM		exit after NUM requests served\n\
	 -c 		do chroot() to DOCROOT\n\
	 -w /path/to 	relative url for uploads	NOTYET\n\
	 -F /path/to	fast-cgi-enable specified path  NOTYET\n\
	 -f SRV:PORT	use fastcgi server		NOTYET\n\
	 -s		enable /server-status URL	SOON\n\
 examples: \n\
    kranker ~/public_html \n\
    kranker -D -t http://people.ee.ethz.ch/~hackerj/kranker_demo.tar\n\
"; // kranker -t my.tar.bz2 -r 0   <-- just to debug tar load behaviour

/* microhttpd */
int MySock;			/* Socket */
int NewSock;			/* Socket for accept. */
socklen_t peer_size = 0;		/* Name stack size for the peer */
//int peer_size = 0;		/* Name stack size for the peer */
struct sockaddr_in addr;	/* Our address */
///*struct*/ socklen_t peer;	/* The peer's address */
struct sockaddr_in peer;	/* The peer's address */
struct hostent *hostentry;	/* Hostentry - for anything we need */
char *request;			/* Our HTTP request */
char *BufIn; //, *BufOut;		/* Input/Output-Buffer */
int BufInLen = 2048, BufOutLen = 2048;
struct arx {
  short port;
  char *basedir;
} args;


void DieWithError(char *errorMessage) {
	printf("%s\n", errorMessage);
	exit(1);
}

int getdoc_id(uint32_t needle) {
	int i;
	for (i=0; i<doccount; i++) {
		if (docinfo[i].hash == needle) {
			return i;
		}
	}
	// if not found, try whether a POST url was given
	// if not found, try whether GET /server-status was requested
	return -404;
}

char* server_status() {
	// fill mem with server-status page
}

int http_request(int Socket, char *req) {
	int docid;
	char *req_get;
	uint32_t cHash;
	char *msg;
	req_get = strtok(req, "\r"); // a bug.
	if (req_get == NULL)
		return -1;

	//if (strlen(req_get) > 100 || strlen(req_get)<1) { 
	//	if (verbose) printf("Bad request.\n");
	//	 return 0; // BAD request...
	//}
	//cHash  = hashlittle(req_get, strlen(req_get), 128);
	cHash  = realpath_cache_key(req_get, strlen(req_get));

	// if cHash == UploadURL && opt_w
	// 	put req into docs[]
	//	msg = b bytes received. get link: ...

	//req_method = strtok(req, " "); ### NO - DO !  hash only!
	//if (hashtab[cHash]) { docid = hashtab[cHash]; printf("New lookup...... %d\n", docid); }
	//printf("HTTP Method  : '%s'\n", req_method);
	docid = getdoc_id(cHash);
	if (docid==-404) {
		printf("404 %s\n", req_get);
		// to be DEFINEed
		char* msg = "HTTP/1.0 404 Not Found\n\n<html><body><h1>Not found</h1>\n\
				The requested document could not be found.\n\
				<hr><i>KRANKER httpd></i></body></html>\n";
		send(Socket, msg, strlen(msg), 0);
		return -1;
	}
	if (verbose) {
		printf("200 %s\n", req_get);
	}
	if (verbose >1 ) {
		printf("HTTP hash    : %x\n", cHash);
		printf("HTTP docid   : %d\n", docid);
		printf("HTTP r size  : %lu\n", docinfo[docid].headersize);
		printf("HTTP b size  : %lu\n", docinfo[docid].filesize);
		printf("HTTP response: \n%s\n", docinfo[docid].header);
	}

	// if docinfo[docid].isdynamic & DOC_IS_FASTCGI
		//
	// if docinfo[docid].isdynamic & DOC_IS_FUNCTION
		// forward to function (store pointer to function?)
	if (docinfo[docid].docmode == DOCMODE_FUNCTION) {
		// currently our only FUNCTION: /server-status
	        int i;
		char* line;
		msg = malloc(32768);
		line = malloc(300);
		// FIXME header!!!!!!!!

		for (i=0; i<doccount; i++) {
			snprintf(line, 300, "<tr> <td>%d</td> <td>%d</td> </tr>\n", docinfo[i].docid, docinfo[i].filesize);
			strcat (msg, line);
		}
		docinfo[docid].filesize = 100;
		docs[docid] = msg;
	}

	sentbytes =+ docinfo[docid].headersize + docinfo[docid].filesize + sentbytes;

	// writev( both )
	if (send(Socket, docinfo[docid].header, docinfo[docid].headersize, 0) < docinfo[docid].headersize) {
		printf("Error while sending %lu response header bytes in line %i!\n",
			 docinfo[docid].headersize, __LINE__ - 1);
		return -1;
	}
	if (send(Socket, docs[docid], docinfo[docid].filesize, 0) < docinfo[docid].filesize) {
		printf("Error while sending %lu response header bytes in line %i!\n",
			 docinfo[docid].filesize, __LINE__ - 1);
		return -1;
	}

	if (docinfo[docid].docmode == DOCMODE_FUNCTION) {
		free(msg);
	}

	return 0;
}

void clear(char *set, size_t len) {
  char *sptr;

  realloc(set, len + 1);

  sptr = set;

  for (sptr = set; sptr < set + len; sptr++) {
    *sptr = '\0';
  }
  return;
}

int main(int argc,char **argv) {
	int c;
	//int docid=0;
	int do_chroot=0;
	char *docrootdir=NULL;
	int usetar=0;
	char *tarpath=NULL;
	char *puturl=NULL;
	struct sigaction sVal;
	pid_t pid, sid;
	//printf("%d\n", sizeof(unsigned long)); exit(0);

	sVal.sa_flags     = SA_SIGINFO;	// 3-var signal-handler
	sVal.sa_sigaction = HandleSignal;	// define sighandler function
	sigaction(SIGINT,  &sVal, NULL);
	sigaction(SIGTERM, &sVal, NULL);
	//sigaction(SIGPIPE, &sVal, NULL);

	BufIn = malloc(2048);
	args.basedir = malloc(124);
	indexdocp = malloc(32);
	strcpy(indexdocp,    "index.html");
	strcpy(args.basedir, "/var/www");

        while ((c = getopt(argc, argv, "cd:i:p:r:t:w:x:vD")) != -1)
                switch (c) {
                        case 'r': maxrequests = atoi(optarg);   break;  
                        case 'c': do_chroot = 1;       		break;  
                        case 'd': docrootdir = optarg;       	break;  
                        case 'i': indexdocp = optarg;       	break;  
                        case 't': tarpath = optarg;	       	break;  
                        case 'w': puturl = optarg;       	break;  
                        case 'v': verbose++;       		break;  
                        case 'D': runasdaemon = 1;     		break;  
                        case 'p': srvport = atoi(optarg);       break;  // docroot relative dir
                        //case 'p': port = optarg;       break;  // http port
                        //case 'x': exitrequests = optarg;       break;  // die after # of requests
		}

        argc -= optind;
        argv += optind;
	// make docroot option work without -d given
	if (argc == 1) { docrootdir = *argv; }

	uid_t uid;
	uid = geteuid();
	if (srvport<1024 && uid!=0) {
		printf("You should be root - given port < 1024.\n");
		exit(0);
	} 

	if (verbose) printf("* KRANKER 0.0.1b starting up with docroot %s\n", docrootdir);
	if (tarpath /*&& !docrootdir*/) {
		// err.... libarchive would allow tarpath=NULL for stdin!
		usetar=1;
	}

	if (!docrootdir && usetar==0) {
		printf("%s", usage_msg);
		exit(1);
	}

	/* open socket */
	MySock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (MySock == (~0)) {
		printf("%s: Error creating server socket.\nErrno was: %i\n", __FILE__, errno);
		exit(1);
	}

	// micro setup
	args.port = srvport;
	args.basedir = docrootdir;

	////// microhttpd ///////////////////////////////////////////
	printf("Setting up server socket: ");
	addr.sin_family = AF_INET;
	printf("AF_INET");
	addr.sin_port = htons(args.port);
	printf(", Port=%i", args.port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY); // FIXME useropt IF !
	printf(", any IP.\n");

	if (bind(MySock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("%s: Error when using bind.\n", __FILE__);
		if (errno == EADDRNOTAVAIL) {
			printf("Address not available.\n");
		} else if (errno == EADDRINUSE) {
			printf("Some other socket is already using the address.\n");
		} else if (errno == EINVAL) {
			printf("The socket already has an address.\n");
		} else if (errno == EACCES) {
			printf("Access to socket denied. (Maybe choose a port > 1024?)\n");
		} else if (errno == EBADF) {
			printf("Bad file descriptor. (?)\n");
		} else {
			printf("Unknown error (errno = %i)\n", errno);
		}
		exit(1);
	}

	printf("Listening for connections on port %i.\n", args.port);
	if (listen(MySock, 10) < 0) {
		printf("%s: Listening error. Errno is %i.\n", __FILE__, errno);
		exit(1);
	}


	if (usetar==1) {
		// open a local or remote (http://) tarfile as documentroot
		if ( kranker_open_tar(tarpath) != 0 ) {
			printf("Failed to open tarfile\n");
			close(MySock);
			exit(1);
		}
	}
	// why not use tar AND dir? :-)
	if (docrootdir) {
		// read documentroot recursively from named directory
		read_docroot(docrootdir);
	}


	if (do_chroot==1) {
		if (chroot(args.basedir) < 0) {
			printf("chroot() failed!\n");
			exit(1);
		}
	}

	chdir("/");

	/* if docroot is ok, daemonize */
	if (runasdaemon == 1) {
		pid = fork();
		if (pid < 0) {
			exit(EXIT_FAILURE);
		} else if (pid > 0) {
			exit(EXIT_SUCCESS);
		}
		
		umask(0);
		sid = setsid();
		if (sid < 0) {
			exit(EXIT_FAILURE);
		}
		// FIXME logging. as stated, something cheap non-local would be nice.
		// maybe collect 10k lines of log, then POST to log server? :-)
	}


	/* running as DAEMON ( if -D ) */
	peer_size = sizeof(peer);



	BufIn = malloc(4096);
	while (conno<maxrequests && (NewSock = accept(MySock, (struct sockaddr *)&peer, &peer_size)) != -1) {
		conno++;
		char host_addr[16];

		//clear(BufIn, BufInLen);
		//BufIn = malloc(1025);
		//clear(BufOut, BufOutLen);
		memset(BufIn,0,4096);

		strcpy(host_addr, inet_ntoa(peer.sin_addr));
		if (verbose>2)
			printf("Connection from %s, port %i.\n", host_addr,
		     		ntohs(peer.sin_port));
		errno = 0;

		if (recv(NewSock, BufIn, 1024, 0) < 0) {
			printf("%s: recv() failed. ", __FILE__);
			if (errno == EBADF) {
				printf("Bad file descriptor. (?)\n");
			} else if (errno == ENOTSOCK) {
				printf("Not a socket. (?)\n");
			} else if (errno == EWOULDBLOCK) {
				printf("Would block nonblocking mode socket.\n");
			} else if (errno == EINTR) {
				printf("Interrupted by a signal.\n");
			} else if (errno == ENOTCONN) {
				printf("Socket disconnected.\n");
				close (NewSock); // wget fix+
				printf("Socket soft shutdown done.\n");
				continue;
			} else {
				printf("Unknown error (errno = %i)\n", errno);
			}
			errno = 0;
		}

		if (BufIn == NULL) {
			printf("Error: HTTP request was NULL!\n");
			send(NewSock, "HTTP/1.0 505\n", 1024, 0);
		} else if (http_request(NewSock, BufIn) < 0) {
			if (verbose>1)
				printf("Socket error %i.\n", errno);
			errno = 0;
		}

		if (verbose>1)
			printf("Host disconnected.\n");

		shutdown(NewSock, 2);
		close (NewSock);
	}
	if (NewSock == -1) {
		printf("Exit from bad accept() run (whatever might have happened).\n");
	}

	close (MySock);
	printf("%s: Cleaning up. Closing socket: ", __FILE__);
	//sleep(1);

	printf("Server: exit.\n");

	////// micro http end //////////////////////////////////////////////////////

	if (verbose) printf("\n* KRANKER going down.\n");
	return 0;
}

/* read docroot into mem, including the response header */
/* redo on HUP */
int read_docroot(char* docdir) {
	struct stat csucks;
	if (verbose) printf("Reading docroot... %s\n", docdir);
	if (chdir(docdir) != 0) {
		printf("  Docroot %s unreadable. Bye.\n", docdir);
		exit(1);
	}
	scan_docs(docdir,".");

	// add modules / "cgis"
	char dname[2] = "/\0";
	char fname[15] = "/server-status\0";
	char /*dnp, *fnp,*/ *np;
	//dnp = &dname;
	//fnp = &fname;
	np = &fname;
	//add_document("/","server-info","","welcher",csucks,DOCMODE_STATIC, DOC_IS_NOINDEX); 
	add_document(NULL,fname,np,np,csucks,DOCMODE_FUNCTION, DOC_IS_NOINDEX); 

	printf("Serving docroot with %lu bytes.\n", (long unsigned)docrootbytes);
	return 1;
}

int scan_docs(char* root, char* dir) {
	DIR *dirp = NULL;
	struct dirent *dp;
	struct stat s;
	char *fn;
        if ((fn = malloc(4096)) == NULL)
                   return 0;

	if (verbose) printf("-- scanning directory: %s\n", dir);
	dirp = opendir(dir);
	if (!dirp)
		return 0;
	while ((dp = readdir(dirp)) != NULL && filecount<MAXFILES) {
		/* if (!has_index && glob(index.*) {add_doc(this) } */
		//sprintf(fn, "%s/%s", dir,dp->d_name);
		if (!strcmp(dp->d_name,".") || !strcmp(dp->d_name,"..") || !strcmp(dp->d_name,".svn") ) {
			continue;
		}
		snprintf(fn, 4096, "%s/%s", dir,dp->d_name);
		if (stat(fn, &s) < 0) {	
			perror(fn);
			exit(1);
		}
		if (S_ISDIR(s.st_mode)) {
			scan_docs(root,fn);
		}
		if (S_ISREG(s.st_mode) && valid_content(fn)) {
			filecount++;
			add_document(root,fn,dir,dp->d_name,s,DOCMODE_MEMORY, DOC_IS_NOINDEX); 
		}
		// (auto)index:
		if (!strcmp(dp->d_name, indexdocp)) {
			add_document(root,fn,dir,dp->d_name,s,DOCMODE_MEMORY, DOC_IS_INDEX); 
		}
	}
	closedir(dirp);
	free(fn);
	return 0;
}

register_document(char* daao) {
	printf("registring doc \n");
}

int add_document(char* root, char* filename, char* dirname, char* filebasename, struct stat s, int docmode, int isindex) {
	FILE  /*tf,*/ *docsrc;
	//char ft[100]; // filetype // FIXME
	int size = 0;
	char *result;
	char conlen[100] = "Content-Length: 0000000000\n\0";
	char getstr[100] = "xxxxyyyyyyyyyyyyxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";;
	char tmpdirname[100] = "\0";
	/*
	  HTTP/1.1 200 OK
	  Last-Modified: Thu, 05 Jan 2006 23:09:46 GMT
	  Accept-Ranges: bytes
	  Content-Length: 12035
	  Keep-Alive: timeout=15, max=29
	  Connection: Keep-Alive
	  Content-Type: image/jpeg
	*/
	/* buggy 
	snprintf(cmd, 4096, "file -bI %s", filename);
	tf = popen(cmd,"r");
	  fgets(ft, sizeof(ft), tf);
	pclose(tf);
	*/
	if (docmode == DOCMODE_FUNCTION) {
		if (verbose) printf("Adding FUNCTION id %4d >> %s \n", doccount, filename);
		snprintf(getstr, sizeof(getstr), "GET %s HTTP/1.0",  filename );
		docinfo[doccount].docmode = DOCMODE_FUNCTION;
		docinfo[doccount].filesize= 0; // header must be sent dynamically
		docinfo[doccount].hash    = realpath_cache_key(getstr, strlen(getstr));
                doccount++;
		snprintf(getstr, sizeof(getstr), "GET %s HTTP/1.1",  filename );
		docinfo[doccount].docmode = DOCMODE_FUNCTION;
		docinfo[doccount].filesize= 0; // header must be sent dynamically
		docinfo[doccount].hash    = realpath_cache_key(getstr, strlen(getstr));
                doccount++;
	}

	if (docmode == DOCMODE_MEMORY) {
		if (verbose) printf("Adding STATIC   id %4d (%s @ %s ) (%s @ %s) (%db) \n",
			 doccount, root, filename, dirname, filebasename, (int)s.st_size /*, ft*/);
		docinfo[doccount].docmode = DOCMODE_MEMORY;
		docsrc = fopen(filename,"rb");
		//docs[doccount] = malloc(s.st_size);
		fseek(docsrc, 0, SEEK_END);
		size = ftell(docsrc);
		fseek(docsrc, 0, SEEK_SET);
		result = (char *)malloc(size+1);
		if (size != fread(result, sizeof(char), size, docsrc))
		{
			free(result);
			return -2; // -2 means file reading fail
		}
		fclose(docsrc);
		docs[doccount] = result;
		//struct docinfo { char* hash; char* url; int docid; };
		docinfo[doccount].docid = doccount;
		docinfo[doccount].filesize = size;

		/* build hash */
		strncpy(tmpdirname, dirname+1, strlen(dirname)-1);
		snprintf(getstr, sizeof(getstr), "GET %s/%s HTTP/1.0",  tmpdirname, filebasename );
		if (isindex==1)
			snprintf(getstr, sizeof(getstr), "GET %s/ HTTP/1.0",  tmpdirname );
		//docinfo[doccount].hash  = hashlittle(getstr, strlen(getstr), 128);
		docinfo[doccount].hash  = realpath_cache_key(getstr, strlen(getstr));
		//hashtab[ docinfo[doccount].hash ] = doccount;
		//realpath_cache_add(const char *path, int path_len, const char *realpath, int realpath_len, time_t t TSRMLS_DC)
		//realpath_cache_add(const char *path, int path_len)
		//realpath_cache_add(getstr, strlen(getstr), doccount);


		/* build header */
		docinfo[doccount].header = (char *)malloc(1024); // "HTTP/1.1 200 OK\n";
		strcpy (docinfo[doccount].header, "HTTP/1.0 200 OK\n");
		snprintf(conlen, sizeof(conlen), "Content-Length: %d\n", size /*, dirname, filebasename*/ );
		strcat (docinfo[doccount].header, conlen);
		strcat (docinfo[doccount].header, "Server: KRANKER/0.0.1b\n");
		//strcat (docinfo[doccount].header, "Connection: close\n");
		// FIXME CONTENT TYPE
		/* finish http header */
		strcat (docinfo[doccount].header, "\n");
		docinfo[doccount].headersize =  strlen(docinfo[doccount].header);
		if (verbose) printf("Adding id %4d, hash: %d,  %8d bytes : %s\n",
			 doccount, docinfo[doccount].hash, (int)s.st_size , getstr);

		doccount++;
		/* FIXME same again for http/1.1 FIXME double ugly */
		docs[doccount] = result;
		docinfo[doccount].docid = doccount-1;
		docinfo[doccount].filesize = size;
		snprintf(getstr, sizeof(getstr), "GET %s/%s HTTP/1.1",  tmpdirname, filebasename );
		if (isindex==1)
			snprintf(getstr, sizeof(getstr), "GET %s/ HTTP/1.1",  tmpdirname );
		//docinfo[doccount].hash  = hashlittle(getstr, strlen(getstr), 128);
		docinfo[doccount].hash  = realpath_cache_key(getstr, strlen(getstr));
		//hashtab[ docinfo[doccount].hash ] = doccount;
		docinfo[doccount].header = (char *)malloc(1024); // "HTTP/1.1 200 OK\n";
		strcpy (docinfo[doccount].header, "HTTP/1.0 200 OK\n");
		snprintf(conlen, sizeof(conlen), "Content-Length: %d\n", size); //, dirname, filebasename
		strcat (docinfo[doccount].header, conlen);
		strcat (docinfo[doccount].header, "Server: KRANKER/0.0.1b\n");
		//strcat (docinfo[doccount].header, "Connection: close\n");
		// finish http header 
		strcat (docinfo[doccount].header, "\n");
		docinfo[doccount].headersize =  strlen(docinfo[doccount].header);
		if (verbose) printf("Adding id %4d, hash: %x,  %8d bytes : %s\n",
			 doccount, docinfo[doccount].hash, (int)s.st_size , getstr);
		/********************************/

		docrootbytes += size;
		doccount++;
	} // end docmode == STATIC



	return 0;
} // end add_document()

int valid_content(char* filename) {
	//printf("Verifying %s\n", filename);
	// run file -bI filename
	return 1;
}

void HandleSignal(int sig, siginfo_t *si, void *context) {
	int res;
	switch (sig) {
		case SIGINT:
			close (MySock);
			printf("SIGINT caught, shutting down.\n");
			printf("%.2f MB sent in %lu requests.\n", (float)sentbytes/1024/1024, conno);
			exit(0);
			break;
			
		case SIGHUP:
			close (MySock);
			printf("SIGHUP caught, shutting down. One day, we might re-read docroot.\n");
			printf("%.2f MB sent in %lu requests.\n", (float)sentbytes/1024/1024, conno);
			exit(0);
			break;
	}
}

