/*
 * KRANKER httpd archive routines (unsing libarchive)
*/
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h> /* for socket(), bind(), and connect() */
#include <arpa/inet.h>  /* for sockaddr_in and inet_ntoa() */
#include <netinet/in.h>
#include <netdb.h>
#include <archive.h>
#include <archive_entry.h>

#define MAXFILES 32768
#define MAXTHREADS 8
#define MAXPENDING 5    /* Maximum outstanding connection requests */

#define DOC_IS_NOINDEX   0
#define DOC_IS_INDEX     1

#define DOCMODE_MEMORY   1      /* static document stored in memory */
#define DOCMODE_FUNCTION 2      /* function provides content (e.g. server-status) */
#define DOCMODE_FASTCGI  3      /* fwd to fastcgi sock */
#define DOCMODE_FILE     4      /* pointer to external file (eeeek) */

// http://schumann.cx/premium-thttpd/

extern int doccount, verbose;
extern size_t  docrootbytes;
extern char *docs[MAXFILES];
extern struct docinfo { unsigned long hash; int docid; char* url; size_t filesize; char* header; size_t headersize; int docmode; };
extern struct docinfo docinfo[MAXFILES];

extern unsigned long realpath_cache_key(const char *path, int path_len);
extern int add_document(char* root, char* filename, char* dirname, char* filebasename, struct stat s, int docmode, int isindex);

int kranker_open_tar_file(char* userpath);
int kranker_open_tar_http(char* userpath);
int myopen(struct archive *a, void *client_data);
int myclose(struct archive *a, void *client_data);
ssize_t myread(struct archive *a, void *client_data, const void **buff);

int kranker_open_tar(char* userpath) {
	int res = -1;
	if (strncmp("http://",userpath, 7)==0) {
		// do the http thing
		printf("Using HTTP to fetch archive from %s as docroot.\n", userpath);
		res = kranker_open_tar_http(userpath);
	} else {
		printf("Using local archive %s as docroot.\n", userpath);
		res = kranker_open_tar_file(userpath);
	}
	return res;
}

// return char* to beginning of archive or NULL
size_t dlsize = 0;
int cit; // crapname. num bytes read after header
char* docdata;
size_t fragsize = 0;

char* parse_response_header(char* header, size_t hsize) {
	char* tok,bak;
	char* foo  = NULL;
	char* ret = NULL;
	char* rh = NULL; // response header
	char* rb = NULL; // response body
	int len,cut,dsize;
	char* conlenstr = NULL; // Content-Length value in string
	conlenstr = malloc(10);

	rh = malloc(2048);
	rb = malloc(2048);
	docdata = malloc(2048);
	memset(rh,0,2048);
	memset(rb,0,2048);
	memset(docdata,0,2048);
	foo = strstr(header, "\n\r\n");
	len = foo-header; //strlen(header)-2;
	memcpy(rh, header, len);
	memcpy(docdata, foo+3,hsize-3);
	//docdata = rb;
	fragsize = hsize-len-3; // i.e. downloaded body bytes
	if (verbose>1)
		printf("header (%d) :\n%s\n--------------\nbody : \n%s\n-------\n", strlen(rh), rh, foo);

	// cut header
	memcpy(header+len,"\0",1);

	tok = strtok(header, "\r\n");
	//printf("tok: '%s'\n", tok);
	if (strcmp(tok,"HTTP/1.1 200 OK")!=0) {
		printf("Non-200 response. Doc not found on remote? Quit.\n");
		return(-1);
	}

	while ((tok=strtok(NULL, "\r\n")) != NULL) {
		//printf("yo: '%s' (%s)\n", tok, "x");
		if (strncmp(tok, "Content-Length: ",15)==0) {
			strncpy(conlenstr, tok+16, strlen(tok)-15);
			// set global dlsize to size of download
			dlsize = atol(conlenstr);
		}
	}
	return rb;
}

char* docstart = NULL; // points to begining of repsonse data (not header)
char* httpbuf;
char* response = NULL;
int sock;

int kranker_open_tar_http(char* userpath) {
	size_t rc;
	FILE* file;
	void* buf;
	char* server=NULL;
	char* target=NULL;
	char* requestb=NULL;
	struct sockaddr_in srv;
	struct hostent *gethostbyname();
	struct hostent *h;
	char *remote;
	requestb = malloc(1024);
	server = strtok(userpath, "/");
	server = strtok(NULL, "/"); // should be "/:" to strip port?
	target = strtok(NULL, "\0");
	snprintf(requestb, 1024, "GET /%s HTTP/1.1\nHost: %s\n\n", target, server);

	//fprintf(stderr,"gethostbyname: cannot find host %s\n",server);
	if ( (sock = socket(AF_INET, SOCK_STREAM, 0)) < 0 ) {
		perror("socket");
		return(-1);
	}
	if ( (h = gethostbyname(server)) == NULL ) {
		fprintf(stderr,"gethostbyname: cannot find host %s\n",server);
		return(-1);
	}
	srv.sin_family = AF_INET;
	srv.sin_port = htons(80);
	memcpy(&srv.sin_addr, h->h_addr, h->h_length);
	if (connect(sock, &srv, sizeof(srv)) < 0 ) {
		perror("connecting stream socket");
		return(-1);
	}

	if (verbose)
		printf("Requesting tar archive: ------\n%s\n------------------------\n", requestb);
	write(sock, requestb, strlen(requestb));

	size_t bytesread = 0;
	size_t tbytesread = 0;
	response = malloc(1448);

	bytesread=read(sock, response, 1448);
	// FIXME check bytesread

	if ((docstart = parse_response_header(response,bytesread))==NULL) {
		printf("Document zero-sized according to http response header. Quit.\n");
		return(-1);
	} else {
		//printf("Downloading %.2f MB...\n", (float)docsize/1024/1024);
		printf("downloading ... %.2f MB \n", (float)dlsize/1024/1024);
	}

        struct archive *a;
	char* result;
        struct archive_entry *entry;
        char getstr[100] = "xxxxyyyyyyyyyyyyxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";;
        a = archive_read_new();
        archive_read_support_compression_all(a);
        archive_read_support_format_all(a);
	httpbuf = malloc(2048); // global ugliness
	memset(httpbuf,0,2048);
        archive_read_open(a, httpbuf, myopen, myread, myclose);
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {

		if (verbose)
			printf("%s\n",archive_entry_pathname(entry));


                // add_document should be better?
                //getname = strtok(userpath, ".");
                //snprintf(getstr, sizeof(getstr), "GET %s HTTP/1.0",  userpath );

		/// DUPLICATED CODE ////////////////////////////////////////////////////////////
                snprintf(getstr, sizeof(getstr), "GET %s HTTP/1.1",  archive_entry_pathname(entry)+1 );
                //printf("%s\n", getstr);
                docinfo[doccount].docmode  = DOCMODE_MEMORY;
                docinfo[doccount].filesize = (size_t) archive_entry_size(entry);
                docinfo[doccount].hash     = realpath_cache_key(getstr, strlen(getstr));
                docinfo[doccount].header = (char *)malloc(1024); // "HTTP/1.1 200 OK\n";
                strcpy (docinfo[doccount].header, "HTTP/1.0 200 OK\n");
                result = (char *)malloc(archive_entry_size(entry)+1);
                archive_read_data(a, result, archive_entry_size(entry));
                docs[doccount] = result;
                //snprintf(conlen, sizeof(conlen), "Content-Length: %lu\n", (long unsigned int)archive_entry_size(entry));
                //strcat (docinfo[doccount].header, conlen);

                strcat (docinfo[doccount].header, "\n");
                docinfo[doccount].headersize =  strlen(docinfo[doccount].header);
                if (verbose) printf("Adding id %4d, hash: %d,  %8d bytes : %s\n",
                         doccount, docinfo[doccount].hash, (size_t) archive_entry_size(entry), getstr);

                printf(" ... %7.2f MB\r", (float)(docrootbytes/1024/1024));
                fflush(stdout);

                doccount++;
                docrootbytes += archive_entry_size(entry);
		/// DUPLICATED CODE ///////////////////////////////////////////////////////////////


		archive_read_data_skip(a);
        }
        archive_read_finish(a);
        //free(mydata);
	
	printf("Loading tar finished.\n");

	close(sock);
	return 0;
}

size_t rcounter = 0;
//FILE *tmp = NULL;
//int tmp = 0;

ssize_t myread(struct archive *a, void *client_data, const void **buff) {
	//char *mydata = client_data;
	size_t bytesread;
	/*if (tmp == NULL) {
		printf("OPENING KOUT\n");
		//tmp = fopen("kout", "w+");
		tmp = open("kout", O_RDWR | O_CREAT | O_TRUNC );
		if (tmp < 1) {
			printf("nodovodooo!!!!!!!!!!!!!!!!!\n");
			perror("open");
		}
	}*/

	if (rcounter ==0) {
		//printf("First read! (%s) (%d)\n", docdata, fragsize);
		rcounter++;
		//write(tmp,"--BEGIN--\n", strlen("--BEGIN--\n"));
		//printf("%d bytes added to kout (THE FIRST)\n", write(tmp, docdata, fragsize));
		//write(tmp,"\n--END--\n", strlen("\n--END--\n"));
		*buff = docdata;
		return fragsize; 
	}
	rcounter++;

        bytesread = read(sock, httpbuf, 1448);
	memcpy(docdata, httpbuf, bytesread);
	//memset(httpbuf,0,);
	//*buff = httpbuf;

	*buff = docdata;
	//write(tmp,"--BEGIN--\n", strlen("--BEGIN--\n"));
	//write(tmp, httpbuf, 1448);
	//write(tmp,"\n--END--\n", strlen("\n--END--\n"));

	//return 0;
	return bytesread;
	//*buff = mydata->buff;
	//return (read(mydata->fd, mydata->buff, 10240));
	//return 0;
}

int myopen(struct archive *a, void *client_data) {
	// struct mydata *mydata = client_data;
	//mydata->fd = open(mydata->name, O_RDONLY);
	//return (mydata->fd >= 0 ? ARCHIVE_OK : ARCHIVE_FATAL);
	//printf("myopen OK !!!!!!!!!!\n");
	return ARCHIVE_OK;
}

int myclose(struct archive *a, void *client_data) {
	//struct mydata *mydata = client_data;
	//printf("closing tmp\n"); //fclose(tmp); close(tmp);
	//if (mydata->fd > 0)
	//close(mydata->fd);
	return (ARCHIVE_OK);
}


///// OPEN LOCAL FILE ////////////////////////////////////////////////////////////
int kranker_open_tar_file(char* userpath) {
char* result;
char* getname, conlen;
struct archive *a;
	struct archive_entry *entry;
	ssize_t bytesread =0;
        char getstr[100] = "xxxxyyyyyyyyyyyyxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";;
	// FIXME currently does not AUTOINDEX

	printf("Reading from archive %s ... \n", userpath);
     	a = archive_read_new();
	archive_read_support_compression_all(a);
	archive_read_support_format_all(a);

	// if not ^http://
	archive_read_open_filename(a, userpath, 4096);
	while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
		// if !plain file 
		// or if archive_entry_size(entry) == 0
		//	archive_read_data_skip(a); continue;

		if (verbose) printf("%s (%lu bytes)\n",
			archive_entry_pathname(entry), (long unsigned int)archive_entry_size(entry));

		// add_document should be better?
		//getname = strtok(userpath, ".");
                //snprintf(getstr, sizeof(getstr), "GET %s HTTP/1.0",  userpath );
                snprintf(getstr, sizeof(getstr), "GET %s HTTP/1.1",  archive_entry_pathname(entry)+1 );
		//printf("%s\n", getstr);

		docinfo[doccount].docmode  = DOCMODE_MEMORY;
		docinfo[doccount].filesize = (size_t) archive_entry_size(entry);
		docinfo[doccount].hash     = realpath_cache_key(getstr, strlen(getstr));
                docinfo[doccount].header = (char *)malloc(1024); // "HTTP/1.1 200 OK\n";
                strcpy (docinfo[doccount].header, "HTTP/1.0 200 OK\n");
                result = (char *)malloc(archive_entry_size(entry)+1);
		archive_read_data(a, result, archive_entry_size(entry));
		docs[doccount] = result;
                //snprintf(conlen, sizeof(conlen), "Content-Length: %lu\n", (long unsigned int)archive_entry_size(entry));
                //strcat (docinfo[doccount].header, conlen);

                strcat (docinfo[doccount].header, "\n");
                docinfo[doccount].headersize =  strlen(docinfo[doccount].header);
                if (verbose) printf("Adding id %4d, hash: %d,  %8d bytes : %s\n",
                         doccount, docinfo[doccount].hash, (size_t) archive_entry_size(entry), getstr);

		printf(" ... %7.2f MB\r", (float)(docrootbytes/1024/1024));
		fflush(stdout);

		doccount++;
		docrootbytes += archive_entry_size(entry);
		archive_read_data_skip(a);
	}

	//larchres = archive_read_support_compression_all();
	printf(" ... %7.2f MB read successfully.\n", (float)(docrootbytes/1024/1024));
	//printf(" ... %lu bytes fetched into docroot.\n", (long unsigned)docrootbytes);

	return 0;
}

