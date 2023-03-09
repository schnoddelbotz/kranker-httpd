/*
   +----------------------------------------------------------------------+
   | PHP Version 5                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2008 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Andi Gutmans <andi@zend.com>                                |
   |          Sascha Schumann <sascha@schumann.cx>                        |
   +----------------------------------------------------------------------+

   |---------------------+
   | stripped down to    |
   | my needs. thx, jan. |
   +---------------------|

*/
/* $Id: tsrm_virtual_cwd.c,v 1.74.2.9.2.38 2007/12/31 07:20:02 sebastian Exp $ */

#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>

#include "tsrm_virtual_cwd.h"
#include "TSRM.h"

	// from _virtual_cwd_globals 
	realpath_cache_bucket *realpath_cache[1024];
        cwd_state cwd;
        long                   realpath_cache_size;
        long                   realpath_cache_size_limit;
        long                   realpath_cache_ttl;


#ifndef HAVE_REALPATH
#define realpath(x,y) strcpy(y,x)
#endif

#define VIRTUAL_CWD_DEBUG 0


/* Only need mutex for popen() in Windows and NetWare because it doesn't chdir() on UNIX */
#ifndef S_ISDIR
#define S_ISDIR(mode) ((mode) & _S_IFDIR)
#endif

#ifndef S_ISREG
#define S_ISREG(mode) ((mode) & _S_IFREG)
#endif

#define TOKENIZER_STRING "/"

/* default macros */

#ifndef IS_DIRECTORY_UP
#define IS_DIRECTORY_UP(element, len) \
	(len == 2 && element[0] == '.' && element[1] == '.')
#endif

#ifndef IS_DIRECTORY_CURRENT
#define IS_DIRECTORY_CURRENT(element, len) \
	(len == 1 && element[0] == '.')
#endif

/* define this to check semantics */
#define IS_DIR_OK(s) (1)
	
#ifndef IS_DIR_OK
#define IS_DIR_OK(state) (php_is_dir_ok(state) == 0)
#endif

unsigned long realpath_cache_key(const char *path, int path_len) 
{
	register unsigned long h;
	const char *e = path + path_len;

	for (h = 2166136261U; path < e;) {
		h *= 16777619;
		h ^= *path++;
	}

	return h;
}

CWD_API void realpath_cache_clean(TSRMLS_D) 
{
	int i;

	for (i = 0; i < sizeof(CWDG(realpath_cache))/sizeof(CWDG(realpath_cache)[0]); i++) {
		realpath_cache_bucket *p = CWDG(realpath_cache)[i];
		while (p != NULL) {
			realpath_cache_bucket *r = p;
			p = p->next;
			free(r);
		}
		CWDG(realpath_cache)[i] = NULL;
	}
	CWDG(realpath_cache_size) = 0;
}

CWD_API void realpath_cache_del(const char *path, int path_len TSRMLS_DC) 
{
	unsigned long key = realpath_cache_key(path, path_len);
	unsigned long n = key % (sizeof(CWDG(realpath_cache)) / sizeof(CWDG(realpath_cache)[0]));
	realpath_cache_bucket **bucket = &CWDG(realpath_cache)[n];

	while (*bucket != NULL) {
		if (key == (*bucket)->key && path_len == (*bucket)->path_len &&
		           memcmp(path, (*bucket)->path, path_len) == 0) {
			realpath_cache_bucket *r = *bucket;
			*bucket = (*bucket)->next;
			CWDG(realpath_cache_size) -= sizeof(realpath_cache_bucket) + r->path_len + 1;
			free(r);
			return;
		} else {
			bucket = &(*bucket)->next;
		}
	}
}

//static inline void realpath_cache_add(const char *path, int path_len, const char *realpath, int realpath_len, time_t t TSRMLS_DC) 
static inline void realpath_cache_add(const char *path, int path_len, int docid /*, const char *realpath, int realpath_len, time_t t TSRMLS_DC*/) 
{
	long size = sizeof(realpath_cache_bucket) + path_len + 1;// + realpath_len + 1;
	if (CWDG(realpath_cache_size) + size <= CWDG(realpath_cache_size_limit)) {
		realpath_cache_bucket *bucket = malloc(size);
		unsigned long n;
	
		bucket->key = realpath_cache_key(path, path_len);
		bucket->path = (char*)bucket + sizeof(realpath_cache_bucket);
		memcpy(bucket->path, path, path_len+1);
		bucket->path_len = path_len;
		//bucket->realpath = bucket->path + (path_len + 1);
		//memcpy(bucket->realpath, realpath, realpath_len+1);
		//bucket->realpath_len = realpath_len;
		bucket->docid = docid;
		//bucket->expires = t + CWDG(realpath_cache_ttl);
		n = bucket->key % (sizeof(CWDG(realpath_cache)) / sizeof(CWDG(realpath_cache)[0]));
		bucket->next = CWDG(realpath_cache)[n];
		CWDG(realpath_cache)[n] = bucket;
		CWDG(realpath_cache_size) += size;
	}
}

static inline realpath_cache_bucket* realpath_cache_find(const char *path, int path_len, time_t t TSRMLS_DC) 
{
	unsigned long key = realpath_cache_key(path, path_len);
	unsigned long n = key % (sizeof(CWDG(realpath_cache)) / sizeof(CWDG(realpath_cache)[0]));
	realpath_cache_bucket **bucket = &CWDG(realpath_cache)[n];

	while (*bucket != NULL) {
	/*
		if (CWDG(realpath_cache_ttl) && (*bucket)->expires < t) {
			realpath_cache_bucket *r = *bucket;
			*bucket = (*bucket)->next;
			CWDG(realpath_cache_size) -= sizeof(realpath_cache_bucket) + r->path_len + 1 + r->realpath_len + 1;
			free(r);
		} else if (key == (*bucket)->key && path_len == (*bucket)->path_len &&
	*/ if (
		           memcmp(path, (*bucket)->path, path_len) == 0) {
			return *bucket;
		} else {
			bucket = &(*bucket)->next;
		}
	}
	return NULL;
}
