/* wikipedia -- thanks:
A Tar file is the concatenation of one or more files. Each file is preceded by a header block. The file data is written unaltered except that its length is rounded up to a multiple of 512 bytes and the extra space is zero filled. The end of an archive is marked by at least two consecutive zero-filled blocks.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>

struct tarfile {
	char*	name;
	int	filecount;
	size_t	size;
	int	blksize;
	char*	curheader;
	char*	curblk;
	size_t	cursize;
};
struct tarheader { // man tar(5)
   char name[100];
   char mode[8];
   char uid[8];
   char gid[8];
   char size[12];
   char mtime[12];
   char checksum[8];
   char typeflag[1];
   char linkname[100];
   char magic[6];
   char version[2];
   char uname[32];
   char gname[32];
   char devmajor[8];
   char devminor[8];
   char prefix[155];
   char pad[12];
};

#define BLKSIZE 512

int main() {
	int fd;
	size_t bread;
	char* buf;
	char* bufend;
	struct tarfile mytar;
	struct tarheader* th;
	buf = malloc(BLKSIZE);
	th  = malloc(BLKSIZE);
	bufend = malloc(255);
	memset(bufend,0,255);
	mytar.filecount = 0;
	//fd = open("cupsdoc.tar", O_RDONLY);
	fd = open("colweb.tar", O_RDONLY);
	printf("s: %d\n", sizeof(th));
	int i;
	while ((bread=read(fd,buf,BLKSIZE)) != 0) {// && mytar.filecount < 5) {
		//th = &buf;
		if (buf[99] == NULL && buf[100] != NULL &&
		    buf[107] == NULL && buf[108] != NULL &&
		    buf[115] == NULL && buf[116] != NULL &&
		    buf[123] == NULL &&
		    buf[257]=='u' && buf[258]=='s' && buf[259]=='t' && buf[260]=='a' && buf[261]=='r'
		) {
			memcpy(th,buf,512); // th = buf
			mytar.cursize = atol(th->size);
			printf("ho! '%s' (size:%d)\n",
					 th->name, mytar.cursize);
			mytar.filecount++;
		}
		//for (i=0; i<BLKSIZE; i++) { printf("%3d : %c\n", i, buf[i]); }
		/*
		if ( memcmp(buf+260,bufend,250)==0) {
				printf("ho! '%s'\n", buf);
				mytar.filecount++;
		}
		*/
	}
	close(fd);
	return fd;
	return 0;
}
