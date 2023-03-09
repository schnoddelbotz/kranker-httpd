CFLAGS = -Wall -O2 -g -I./libarchive-2.4.17/libarchive -ftest-coverage -fprofile-arcs
LDFLAGS = -L./libarchive-2.4.17/.libs
#CFLAGS = -g
LARCHIVE = libarchive-2.4.17


#kranker: hash.o kranker.o
kranker: larchive tsrm_virtual_cwd.o xtar.o kranker.o
	#$(CC) $(CFLAGS) -o kranker hash.o kranker.o
	#$(CC) $(CFLAGS) $(LDFLAGS) -larchive -o kranker kranker.o tsrm_virtual_cwd.o xtar.o
	$(CC) $(CFLAGS) $(LDFLAGS) -static -larchive -o kranker kranker.o tsrm_virtual_cwd.o xtar.o ./libarchive-2.4.17/.libs/libarchive.a -lbz2 -lz

larchive:
	(  cd $(LARCHIVE) ; [ -d .libs ] || ( ./configure && make ) )

hash.o: hash.c
	$(CC) $(CFLAGS) -c -o $@ $<

xtar.o: xtar.c
	$(CC) $(CFLAGS) -c -o $@ $<

kranker.o: kranker.c
	$(CC) $(CFLAGS) -c -o $@ $<

tsrm_virtual_cwd.o: tsrm_virtual_cwd.c
	$(CC) $(CFLAGS) -c -o $@ $<

TD=www.test.com
www.test.com:
	( mkdir $(TD) && cd $(TD) && tar -xvf ../www.test.com.tar )
	( cd $(TD) && tar -cvzf ../www.test.com.tar.gz )
	( cd $(TD) && tar -cvjf ../www.test.com.tar.bz2 )

# let him serve 3 requests (1 index, 1 image, 1 404)
test: kranker www.test.com
	./kranker -D -r 6000 -d www.test.com 
	ab -n 1000 http://127.0.0.1:8080/32b.random
	ab -n 1000 http://127.0.0.1:8080/64b.random
	ab -n 1000 http://127.0.0.1:8080/128b.random
	ab -n 1000 http://127.0.0.1:8080/512b.random
	ab -n 1000 http://127.0.0.1:8080/1024b.random
	ab -n 1000 http://127.0.0.1:8080/1m.random
	@echo "6000 test requests done. kranker should shut down..."

realclean: clean
	( cd $(LARCHIVE) && make clean )
	rm -rf www.test.com
	rm -f www.test.com.tar.gz www.test.com.tar.bz2

clean:
	rm -f kranker kranker.o hash.o xtar.o
	rm -rf www.test.com
