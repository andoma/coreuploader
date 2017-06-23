#include <fcntl.h>
#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>

#include "bearssl.h"
#include "ta.h"


static int
sock_read(void *ctx, unsigned char *buf, size_t len)
{
  for (;;) {
    ssize_t rlen;

    rlen = read(*(int *)ctx, buf, len);
    if (rlen <= 0) {
      if (rlen < 0 && errno == EINTR) {
        continue;
      }
      return -1;
    }
    return (int)rlen;
  }
}

static int
sock_write(void *ctx, const unsigned char *buf, size_t len)
{
  for (;;) {
    ssize_t wlen;

    wlen = write(*(int *)ctx, buf, len);
    if (wlen <= 0) {
      if (wlen < 0 && errno == EINTR) {
        continue;
      }
      return -1;
    }
    return (int)wlen;
  }
}



int
main(int argc, char **argv)
{
  br_ssl_client_context sc;
  br_x509_minimal_context xc;
  unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
  uint8_t readbuf[32768];
  br_sslio_context ioc;
  const char *host;
  const char *system = NULL;
  const char *bucket = NULL;
  const char *filename = NULL;
  char path[2048];
  int c;
  int inputfd = 0;

  while((c = getopt(argc, argv, "s:b:f:i:")) != -1) {
    switch(c) {
    case 's':
      system = optarg;
      break;
    case 'b':
      bucket = optarg;
      break;
    case 'f':
      filename = optarg;
      break;
    case 'i':
      inputfd = open(optarg, O_RDONLY);
      if(inputfd == -1) {
        fprintf(stderr, "Unable to open %s -- %s\n", optarg, strerror(errno));
        exit(1);
      }
      break;
    }
  }

  openlog("coreuploader", LOG_PID | LOG_NDELAY, LOG_DAEMON);

  if(system == NULL) {
    syslog(LOG_ERR, "No system given");
    exit(1);
  }

  if(bucket == NULL) {
    syslog(LOG_ERR, "No bucket given");
    exit(1);
  }

  if(filename == NULL) {
    syslog(LOG_ERR, "No filename given");
    exit(1);
  }

  if(!strcmp(system, "gcp")) {
    host = "www.googleapis.com";
    snprintf(path, sizeof(path),
             "/upload/storage/v1/b/%s/o?uploadType=media&name=%s",
             bucket, filename);
  } else {
    syslog(LOG_ERR, "System (-s) '%s' not recognized", system);
    exit(1);
  }

  signal(SIGPIPE, SIG_IGN);

  struct hostent *h = gethostbyname(host);
  if(h == NULL) {
    syslog(LOG_ERR, "Unable to resolve %s -- %s", host, hstrerror(h_errno));
    exit(1);
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd == -1) {
    syslog(LOG_ERR, "Unable to create socket -- %s", strerror(errno));
    exit(1);
  }

  struct sockaddr_in sin = {
    .sin_family = AF_INET,
    .sin_port = htons(443),
  };
  memcpy(&sin.sin_addr, h->h_addr, 4);
  int r = connect(fd, (struct sockaddr *)&sin, sizeof(struct sockaddr_in));
  if(r == -1) {
    syslog(LOG_ERR, "Unable to connect to %s -- %s", host, strerror(errno));
    exit(1);
  }

  br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
  br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 1);
  br_ssl_client_reset(&sc, host, 0);
  br_sslio_init(&ioc, &sc.eng, sock_read, &fd, sock_write, &fd);

#define wrstr(x) br_sslio_write_all(&ioc, x, strlen(x));


  wrstr("POST ");
  wrstr(path);
  wrstr(" HTTP/1.0\r\nHost: ");
  wrstr(host);
  wrstr("\r\nTransfer-Encoding: chunked\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n");
  br_sslio_flush(&ioc);

  while(1) {
    int bytes = read(inputfd, readbuf, sizeof(readbuf));
    if(bytes <= 0)
      break;

    char chunkstr[64];
    snprintf(chunkstr, sizeof(chunkstr), "%x\r\n", bytes);
    wrstr(chunkstr);
    br_sslio_write_all(&ioc, readbuf, bytes);
    wrstr("\r\n");
  }
  wrstr("0\r\n\r\n");
  br_sslio_flush(&ioc);

  int line_num = 0;
  while(1) {
    int rlen;
    char tmp[512];

    rlen = br_sslio_read(&ioc, tmp, sizeof(tmp));
    if (rlen < 0) {
      break;
    }

    if(line_num == 0) {
      tmp[strcspn(tmp, "\r\n")] = 0;
      syslog(LOG_NOTICE, "Uploaded core %s -- %s", filename, tmp);
    }
    line_num++;
  }

  close(fd);

  return 0;
}
