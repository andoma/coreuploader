
coreuploader: coreuploader.c ta.h BearSSL/build/libbearssl.a
	gcc -IBearSSL/inc -Wall -O2 -o $@ $< BearSSL/build/libbearssl.a

ta.h: cacert.pem BearSSL/build/libbearssl.a
	BearSSL/build/brssl ta $< >$@

cacert.pem:
	 curl -O https://curl.haxx.se/ca/cacert.pem

BearSSL/build/libbearssl.a:
	make -C BearSSL

