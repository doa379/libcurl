// $ cc multi-uv.c -Wall -pie -l uv -l curl -o multi-uv && ./multi-uv

#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include <curl/curl.h>

typedef struct
{
  uv_poll_t poll_handle;
  curl_socket_t sockfd;
  void *userp;
} curl_context_t;

typedef struct
{
  uv_loop_t *loop;
  uv_timer_t timeout;
  CURLM *curl_handle;
} Stream;

typedef struct
{
  Stream *st;
} A;

static void curl_close_cb(uv_handle_t *handle)
{
  curl_context_t *context = (curl_context_t *) handle->data;
  free(context);
}

static void destroy_curl_context(curl_context_t *context)
{
  uv_close((uv_handle_t *) &context->poll_handle, curl_close_cb);
}

static void check_multi_info(A *a)
{
  char *done_url;
  CURLMsg *message;
  int pending;
  CURL *easy_handle;

  while((message = curl_multi_info_read(a->st->curl_handle, &pending))) {
    switch(message->msg) {
    case CURLMSG_DONE:
      easy_handle = message->easy_handle;
      curl_easy_getinfo(easy_handle, CURLINFO_EFFECTIVE_URL, &done_url);
      printf("%s DONE\n", done_url);
      curl_multi_remove_handle(a->st->curl_handle, easy_handle);
      curl_easy_cleanup(easy_handle);
      break;

    default:
      fprintf(stderr, "CURLMSG default\n");
      break;
    }
  }
}

static curl_context_t *create_curl_context(curl_socket_t sockfd, A *a)
{
  curl_context_t *context = malloc(sizeof(curl_context_t));
  context->sockfd = sockfd;
  context->userp = (A *) a;
  uv_poll_init_socket(a->st->loop, &context->poll_handle, sockfd);
  context->poll_handle.data = context;
  return context;
}

static void curl_perform(uv_poll_t *req, int status, int events)
{
  int running_handles;
  int flags = 0;
  curl_context_t *context = req->data;
  A *a = (A *) context->userp;

  if(events & UV_READABLE) flags |= CURL_CSELECT_IN;
  if(events & UV_WRITABLE) flags |= CURL_CSELECT_OUT;

  curl_multi_socket_action(a->st->curl_handle, context->sockfd, flags, &running_handles);

  printf("curl_perform(): &*a = %p\n", a);
  check_multi_info(a);
}

static int handle_socket(CURL *easy, curl_socket_t s, int action, void *userp, void *socketp)
{
  A *a = (A *) userp;
  curl_context_t *curl_context;
  int events = 0;

  switch(action) {
  case CURL_POLL_IN:
  case CURL_POLL_OUT:
  case CURL_POLL_INOUT:
    curl_context = socketp ? (curl_context_t *) socketp : create_curl_context(s, a);
    curl_multi_assign(a->st->curl_handle, s, (void *) curl_context);

    if(action != CURL_POLL_IN) events |= UV_WRITABLE;
    if(action != CURL_POLL_OUT) events |= UV_READABLE;

    uv_poll_start(&curl_context->poll_handle, events, curl_perform);
    break;

  case CURL_POLL_REMOVE:
    if(socketp) {
      uv_poll_stop(&((curl_context_t*)socketp)->poll_handle);
      destroy_curl_context((curl_context_t*) socketp);
      curl_multi_assign(a->st->curl_handle, s, NULL);
    }
    break;

  default:
    abort();
  }

  return 0;
}

static void on_timeout(uv_timer_t *req)
{
  int running_handles;
  A *a = (A *) req->data;
  curl_multi_socket_action(a->st->curl_handle, CURL_SOCKET_TIMEOUT, 0, &running_handles);
  printf("on_timeout(): &*a = %p\n", a);
  check_multi_info(a);
}

static int start_timeout(CURLM *multi, long timeout_ms, void *userp)
{
  A *a = (A *) userp;

  if(timeout_ms < 0) {
    uv_timer_stop(&a->st->timeout);
  }
  else {
    if(timeout_ms == 0)
      timeout_ms = 1; /* 0 means directly call socket_action, but we'll do it
                         in a bit */
    a->st->timeout.data = a;
    uv_timer_start(&a->st->timeout, on_timeout, timeout_ms, 0);
  }
  return 0;
}

void perform_request(A *a)
{
  char buffer[8192];
  buffer[0] = '\0';
  printf("perform_request(): &*a = %p\n", a);

  a->st->loop = uv_default_loop();
  curl_global_init(CURL_GLOBAL_ALL);
  uv_timer_init(a->st->loop, &a->st->timeout);
  a->st->curl_handle = curl_multi_init();

  curl_multi_setopt(a->st->curl_handle, CURLMOPT_SOCKETFUNCTION, handle_socket);
  curl_multi_setopt(a->st->curl_handle, CURLMOPT_SOCKETDATA, (A *) a);
  curl_multi_setopt(a->st->curl_handle, CURLMOPT_TIMERFUNCTION, start_timeout);
  curl_multi_setopt(a->st->curl_handle, CURLMOPT_TIMERDATA, (A *) a);

  CURL *easy_handle = curl_easy_init();
  curl_easy_setopt(easy_handle, CURLOPT_WRITEDATA, buffer);
  curl_easy_setopt(easy_handle, CURLOPT_URL, "192.168.1.1");
  curl_multi_add_handle(a->st->curl_handle, easy_handle);
  uv_run(a->st->loop, UV_RUN_DEFAULT);
  
  printf("%s\n", buffer);
  curl_multi_cleanup(a->st->curl_handle);
  curl_global_cleanup();
}

int main()
{
  A a;
  Stream s;
  a.st = &s;
  printf("main(): &a = %p\n", &a);
  perform_request(&a);
  return 0;
}
