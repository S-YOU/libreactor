#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <time.h>
#include <string.h>
#include <netdb.h>
#include <err.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/queue.h>

#include <dynamic.h>

#include "reactor_util.h"
#include "reactor_user.h"
#include "reactor_pool.h"
#include "reactor_core.h"
#include "reactor_timer.h"
#include "reactor_resolver.h"
#include "reactor_stream.h"
#include "reactor_tcp.h"
#include "reactor_http_parser.h"
#include "reactor_http.h"
#include "reactor_http_server.h"

static void reactor_http_server_map_release(void *data)
{
  reactor_http_server_map *map;

  map = data;
  free(map->method);
  free(map->path);
}

static void reactor_http_server_hold(reactor_http_server *server)
{
  server->ref ++;
}

static void reactor_http_server_release(reactor_http_server *server)
{
  server->ref --;
  if (reactor_unlikely(!server->ref))
    {
      vector_destruct(&server->map);
      if (server->user.callback)
        reactor_user_dispatch(&server->user, REACTOR_HTTP_SERVER_EVENT_CLOSE, server);
    }
}

static void reactor_http_server_error(reactor_http_server *server)
{
  if (server->user.callback)
    reactor_user_dispatch(&server->user, REACTOR_HTTP_SERVER_EVENT_ERROR, server);
}

static void reactor_http_server_date(reactor_http_server *server)
{
  static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  time_t t;
  struct tm tm;

  (void) time(&t);
  (void) gmtime_r(&t, &tm);
  (void) strftime(server->date, sizeof server->date, "---, %d --- %Y %H:%M:%S GMT", &tm);
  memcpy(server->date, days[tm.tm_wday], 3);
  memcpy(server->date + 8, months[tm.tm_mon], 3);
}

static void reactor_http_server_session_hold(reactor_http_server_session *session)
{
  session->ref ++;
}

static void reactor_http_server_session_release(reactor_http_server_session *session)
{
  session->ref --;
  if (!session->ref)
    {
      reactor_http_server_release(session->server);
      free(session);
    }
}

static void reactor_http_server_session_request(reactor_http_server_session *session, reactor_http_request *request)
{
  reactor_http_server_map *map;
  size_t i;

  map = vector_data(&session->server->map);
  for (i = 0; i < vector_size(&session->server->map); i ++)
    if ((!map[i].method || strcmp(map[i].method, request->method) == 0) &&
        (!map[i].path || strcmp(map[i].path, request->path) == 0))
      {
        reactor_http_server_session_hold(session);
        reactor_user_dispatch(&map[i].user, REACTOR_HTTP_SERVER_EVENT_REQUEST,
                              (reactor_http_server_context[]){{.session = session, .request = request}});
        return;
      }
}

static void reactor_http_server_session_event(void *state, int type, void *data)
{
  reactor_http_server_session *session = state;
  reactor_http_request *request;

  switch (type)
    {
    case REACTOR_HTTP_EVENT_HANGUP:
    case REACTOR_HTTP_EVENT_ERROR:
      session->state = REACTOR_HTTP_SERVER_SESSION_STATE_CLOSED;
      reactor_http_close(&session->http);
      break;
    case REACTOR_HTTP_EVENT_CLOSE:
      reactor_http_server_session_release(session);
      break;
    case REACTOR_HTTP_EVENT_REQUEST:
      request = data;
      reactor_http_server_session_request(session, request);
      break;
    }
}

static void reactor_http_server_session_create(reactor_http_server *server, int socket)
{
  reactor_http_server_session *session;

  session = calloc(1, sizeof *session);
  session->state = REACTOR_HTTP_SERVER_SESSION_STATE_OPEN;
  session->server = server;
  reactor_http_server_hold(server);
  reactor_http_server_session_hold(session);
  reactor_http_open(&session->http, reactor_http_server_session_event, session, socket, REACTOR_HTTP_FLAG_SERVER);
}

static void reactor_http_server_event_tcp(void *state, int type, void *data)
{
  reactor_http_server *server = state;

  switch (type)
    {
    case REACTOR_TCP_EVENT_ERROR:
      reactor_http_server_error(server);
      break;
    case REACTOR_TCP_EVENT_ACCEPT:
      reactor_http_server_session_create(server, *(int *) data);
      break;
  }
}

static void reactor_http_server_event_timer(void *state, int type, void *data)
{
  reactor_http_server *server = state;

  (void) data;
  switch (type)
    {
    case REACTOR_TIMER_EVENT_ERROR:
      reactor_http_server_error(server);
      break;
    case REACTOR_TIMER_EVENT_CALL:
      reactor_http_server_date(server);
      break;
    }
}

void reactor_http_server_open(reactor_http_server *server, reactor_user_callback *callback, void *state,
                              char *host, char *port)
{
  server->ref = 0;
  server->state = REACTOR_HTTP_SERVER_STATE_OPEN;
  vector_construct(&server->map, sizeof(reactor_http_server_map));
  vector_object_release(&server->map, reactor_http_server_map_release);
  reactor_user_construct(&server->user, callback, state);
  server->name = "*";
  reactor_http_server_date(server);
  reactor_http_server_hold(server);
  reactor_timer_open(&server->timer, reactor_http_server_event_timer, server, 1000000000, 1000000000);
  reactor_tcp_open(&server->tcp, reactor_http_server_event_tcp, server, host, port, REACTOR_HTTP_FLAG_SERVER);
}

void reactor_http_server_close(reactor_http_server *server)
{
  (void) server;
  /*
  if (reactor_http_server_active(server))
    {
      server->flags &= ~REACTOR_HTTP_SERVER_FLAG_ACTIVE;
      if (reactor_tcp_active(&server->tcp))
        reactor_tcp_close(&server->tcp);
      if (reactor_timer_active(&server->timer))
        reactor_timer_close(&server->timer);
    }
  */
}

void reactor_http_server_route(reactor_http_server *server, reactor_user_callback *callback, void *state,
                               char *method, char *path)
{
  vector_push_back(&server->map, (reactor_http_server_map[]) {{
        .user = {.callback = callback, .state = state},
        .method = method,
        .path = path}});
}

void reactor_http_server_respond_mime(reactor_http_server_session *session, char *type, char *data, size_t size)
{
  if (session->state & REACTOR_HTTP_SERVER_SESSION_STATE_OPEN)
    {
      reactor_http_write_response(&session->http, (reactor_http_response[]){{1, 200, "OK",
              3, (reactor_http_header[]){
              {"Server", session->server->name},
                {"Date", session->server->date},
                  {"Content-Type", type}
            }, data, size}});
      reactor_http_flush(&session->http);
    }
  reactor_http_server_session_release(session);
}

void reactor_http_server_respond_text(reactor_http_server_session *session, char *text)
{
  reactor_http_server_respond_mime(session, "text/plain", text, strlen(text));
}
