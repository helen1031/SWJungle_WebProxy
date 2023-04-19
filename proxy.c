#include "csapp.h"
#include <stdio.h>

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

sem_t mutex;

struct cache {
  char uri[MAXLINE];
  char object[MAX_OBJECT_SIZE];
  int size;
  struct cache *prev;
  struct cache *next;
  time_t timestamp;
};

struct cache *cache_list = NULL;
int cache_size = 0;

void modify_http_header(char *http_header, char *hostname, int port, char *path, rio_t *server_rio);
void parse_uri(char *uri, char *host, char *port, char *path);
void doit(int fd);
void *thread(void *vargp);
struct cache *cache_lookup(char *uri);
void cache_insert(char *uri, char *object, int size);
void cache_remove();

int main(int argc, char **argv) {
  //printf("%s", user_agent_hdr);
  //return 0;
  int listenfd, connfd;
  int *connfdp;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage server_addr;
  struct sockaddr_storage client_addr;
  pthread_t tid;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  listenfd = Open_listenfd(argv[1]);

  Sem_init(&mutex, 0, 1); // mutex를 1로 초기화

  while (1) {
    clientlen = sizeof(client_addr);
    connfdp = Malloc(sizeof(int));
    *connfdp = Accept(listenfd, (SA *)&client_addr, &clientlen);
    Getnameinfo((SA *)&client_addr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
    printf("Accepted connection from (%s, %s)\n", hostname, port);
    Pthread_create(&tid, NULL, thread, connfdp);
      
  }
  printf("%s", user_agent_hdr);
  return 0;
}

void *thread(void *vargp) {
  int connfd = *((int *)vargp);
  Pthread_detach(pthread_self());
  Free(vargp);
  doit(connfd);   // line:netp:tiny:doit
  Close(connfd);
  return NULL;
}

struct cache *cache_lookup(char *uri) {
  P(&mutex);
  struct cache *ptr = cache_list;
  while (ptr != NULL) {
    if (strcmp(ptr -> uri, uri) == 0) {
      // 현재 시간으로 업데이트
      ptr -> timestamp = time(NULL);
      V(&mutex);
      return ptr;
    }
    ptr = ptr -> next;
  }
  V(&mutex);
  return NULL;
}

void cache_insert(char *uri, char *object, int size) {
  P(&mutex);
  struct cache *new_cache = malloc(sizeof(struct cache));
  strcpy(new_cache->uri, uri);
  strcpy(new_cache->object, object);
  new_cache -> size = size;
  // list 맨앞에 삽입한다
  new_cache -> prev = NULL;
  new_cache -> next = cache_list;
  if (cache_list != NULL) {
    cache_list -> prev = new_cache;
  }
  cache_list = new_cache;
  cache_size = cache_size + size;

  new_cache -> timestamp = time(NULL);

  while(cache_size > MAX_CACHE_SIZE) {
    cache_remove();
  }
  V(&mutex);
}

// 캐시에서 쫓아낼 친구를 찾는다 - LRU 사용
void cache_remove() {
  P(&mutex);
  struct cache *ptr = cache_list;
  time_t oldest_time = time(NULL);
  struct cache *oldest = NULL;

  while (ptr != NULL) {
    if (ptr -> timestamp < oldest_time) {
      oldest = ptr;
      oldest_time = oldest -> timestamp;
    }
    ptr = ptr -> next;
  }

  if (oldest != NULL) {
    // 맨 앞 삭제
    if (oldest -> prev == NULL) {
      oldest -> next -> prev = NULL;
      cache_list = oldest -> next;
    }
    // 맨 뒤 삭제
    else if (oldest -> next == NULL) {
      oldest -> prev -> next = NULL;
      oldest -> prev = NULL;
    }
    // 중간 부분 삭제 
    else {
      oldest -> prev -> next = oldest -> next;
      oldest -> next -> prev = oldest -> prev;
    }
    cache_size -= oldest -> size;
    free(oldest);
  }
  V(&mutex);
}

void doit(int fd) {
  int finalfd; // proxy 서버가 client로서 최종 서버에게 보내는 소켓 식별자
  char client_buf[MAXLINE], server_buf[MAXLINE];

  char method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char host[MAXLINE], path[MAXLINE], port[MAXLINE];
  rio_t client_rio, server_rio;

  // 1. 클라이언트로부터 요청을 수신한다 : client - proxy(server 역할)
  Rio_readinitb(&client_rio, fd);
  Rio_readlineb(&client_rio, client_buf, MAXLINE);      // 클라이언트 요청 읽고 파싱
  sscanf(client_buf, "%s %s %s", method, uri, version);
  printf("===FROM CLIENT===\n");
  printf("Request headers:\n");
  printf("%s", client_buf);
  
  if (strcasecmp(method, "GET")) {  // GET 요청만 처리한다
    printf("Proxy does not implement the method\n");
    return; 
  }

  // 캐시 체크하기 - 있다면 캐시 히트
  P(&mutex);
  struct cache *cached_item = cache_lookup(uri);
  if (cached_item != NULL) {
    printf("===CACHE HIT===\n");
    printf("Proxy read %d bytes from cache and sent to client\n", cached_item->size);
    Rio_writen(fd, cached_item->object, cached_item->size);
    return;
  }
  V(&mutex);

  // 2. 요청에서 목적지 서버의 호스트 및 포트 정보를 추출한다
  parse_uri(uri, host, port, path);

  // Proxy 서버: client 서버로서의 역할 수행
  // 3. 추출한 호스트 및 포트 정보를 사용하여 목적지 서버로 요청을 날린다
  finalfd = Open_clientfd(host, port);   // finalfd = proxy가 client로서 목적지 서버로 보낼 소켓 식별자
  sprintf(server_buf, "%s %s %s\r\n", method, path, version);
  printf("===TO SERVER===\n");
  printf("%s\n", server_buf);
  
  Rio_readinitb(&server_rio, finalfd);
  modify_http_header(server_buf, host, port, path, &client_rio);  // 클라이언트로부터 받은 요청의 헤더를 수정하여 보냄
  Rio_writen(finalfd, server_buf, strlen(server_buf));

  // 4. 서버로부터 응답을 읽어 클라이언트에 반환한다
  char cache_buf[MAX_OBJECT_SIZE];
  int cache_buf_size = 0;
  size_t n;
  while((n = Rio_readlineb(&server_rio, server_buf, MAXLINE)) != 0) {
    printf("Proxy received %d bytes from server and sent to client\n", n);
    Rio_writen(fd, server_buf, n);

    cache_buf_size += n;
    if (cache_buf_size < MAX_OBJECT_SIZE) {
      strcat(cache_buf, server_buf);
    }
  }
  Close(finalfd);

  P(&mutex);
  if (cache_size < MAX_OBJECT_SIZE) {
    printf("===CACHE INSERT===\n");
    cache_insert(uri, cache_buf, cache_buf_size);
  }
  V(&mutex);
}

// 목적지 서버에 보낼 HTTP 요청 헤더로 수정하기
void modify_http_header(char *http_header, char *hostname, int port, char *path, rio_t *client_rio) {
  char buf[MAXLINE], request_hdr[MAXLINE], other_hdr[MAXLINE], host_hdr[MAXLINE];

  sprintf(request_hdr, "GET %s HTTP/1.0\r\n", path);

  while(1) {
    Rio_readlineb(client_rio, buf, MAXLINE);

    if (strcmp(buf, "\r\n") == 0) break;

    if (strncasecmp(buf, "Host", strlen("Host")) == 0)  // Host: 
    { 
      strcpy(host_hdr, buf);
      continue;
    }

    if ((strncasecmp(buf, "Connection", strlen("Connection")) != 0)
        && (strncasecmp(buf, "Proxy-Connection", strlen("Proxy-Connection")) != 0 )
        && (strncasecmp(buf, "User-Agent", strlen("User-Agent")) != 0))
    {
      strcat(other_hdr, buf);
    }
  }
  if (strlen(host_hdr) == 0) {
    sprintf(host_hdr, "Host: %s\r\n", hostname);
  }
  sprintf(http_header, "%s%s%s%s%s%s%s", 
          request_hdr, 
          host_hdr, 
          "Connection: close\r\n", 
          "Proxy-Connection: close\r\n", 
          user_agent_hdr, 
          other_hdr, 
          "\r\n");
  printf("%s%s%s%s%s%s%s", request_hdr, host_hdr, "Connection: close\r\n", "Proxy-Connection: close\r\n", user_agent_hdr, other_hdr, "\r\n");
  return;
}

void parse_uri(char *uri, char *host, char* port, char *path) {
  // http://hostname:port/path 형태
  char *ptr = strstr(uri, "//");
  ptr = ptr != NULL ? ptr + 2 : uri;  // http:// 생략
  char *host_ptr = ptr;               // host 부분 찾기
  char *port_ptr = strchr(ptr, ':');  // port 부분 찾기
  char *path_ptr = strchr(ptr, '/');  // path 부분 찾기
  
  // 포트가 있는 경우
  if (port_ptr != NULL && (path_ptr == NULL || port_ptr < path_ptr)) {
    strncpy(host, host_ptr, port_ptr - host_ptr); // 버퍼, 복사할 문자열, 복사할 길이
    strncpy(port, port_ptr + 1, path_ptr - port_ptr - 1);
  }
  // 포트가 없는 경우 
  else {
    strcpy(port, "80"); // 기본값
    strncpy(host, host_ptr, path_ptr - host_ptr);
  }
  strcpy(path, path_ptr);
  return;
}