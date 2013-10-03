/*
 * This file is part of libwebsock
 *
 * Copyright (C) 2012 Payden Sutherland
 *
 * libwebsock is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3 of the License.
 *
 * libwebsock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libwebsock; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "websock.h"
#include "sha1.h"
#include "base64.h"

static inline int
libwebsock_read_header(libwebsock_frame *frame)
{
  int i, new_size;
  enum WS_FRAME_STATE state;

  state = frame->state;
  switch (state) {
    case sw_start:
      if (frame->rawdata_idx < 2) {
        return 0;
      }
      frame->state = sw_got_two;
    case sw_got_two:
      frame->mask_offset = 2;
      frame->fin = (*(frame->rawdata) & 0x80) == 0x80 ? 1 : 0;
      frame->opcode = *(frame->rawdata) & 0xf;
      frame->payload_len_short = *(frame->rawdata + 1) & 0x7f;
      frame->state = sw_got_short_len;
      if ((*(frame->rawdata) & 0x70) != 0) {  //some reserved bits set
        return -1;
      }
      if ((*(frame->rawdata) & 0xf8) == 0x08) { //continuation control frame. invalid.
        return -1;
      }
      if ((*(frame->rawdata + 1) & 0x80) != 0x80) {
        return -1;
      }
    case sw_got_short_len:
      switch (frame->payload_len_short) {
        case 126:
          if (frame->rawdata_idx < 4) {
            return 0;
          }
          frame->mask_offset += 2;
          frame->payload_offset = frame->mask_offset + MASK_LENGTH;
          frame->payload_len = ntohs(*((unsigned short int *)(frame->rawdata+2)));
          frame->state = sw_got_full_len;
          break;
        case 127:
          if (frame->rawdata_idx < 10) {
            return 0;
          }
          frame->mask_offset += 8;
          frame->payload_offset = frame->mask_offset + MASK_LENGTH;
          frame->payload_len = ntohl(*((unsigned int *)(frame->rawdata+6)));
          frame->state = sw_got_full_len;
          break;
        default:
          frame->payload_len = frame->payload_len_short;
          frame->payload_offset = frame->mask_offset + MASK_LENGTH;
          frame->state = sw_got_full_len;
          break;
      }
    case sw_got_full_len:
      if (frame->rawdata_idx < frame->payload_offset) {
        return 0;
      }
      for (i = 0; i < MASK_LENGTH; i++) {
        frame->mask[i] = *(frame->rawdata + frame->mask_offset + i) & 0xff;
      }
      frame->state = sw_loaded_mask;
      frame->size = frame->payload_offset + frame->payload_len;
      if (frame->size > frame->rawdata_sz) {
        new_size = frame->size;
        new_size--;
        new_size |= new_size >> 1;
        new_size |= new_size >> 2;
        new_size |= new_size >> 4;
        new_size |= new_size >> 8;
        new_size |= new_size >> 16;
        new_size++;
        frame->rawdata_sz = new_size;
        frame->rawdata = (char *) realloc(frame->rawdata, new_size);
      }
      return 1;
    case sw_loaded_mask:
      return 1;
  }
  return 0;
}

void
libwebsock_handle_signal(evutil_socket_t sig, short event, void *ptr)
{
  libwebsock_context *ctx = ptr;
  event_base_loopexit(ctx->base, NULL);
}

void
libwebsock_populate_close_info_from_frame(libwebsock_close_info **info, libwebsock_frame *close_frame)
{
  libwebsock_close_info *new_info;
  unsigned short code_be;
  int at_most;

  if (close_frame->payload_len < 2) {
    return;
  }

  new_info = (libwebsock_close_info *) malloc(sizeof(libwebsock_close_info));
  if (!new_info) {
    fprintf(stderr, "Error allocating memory for libwebsock_close_info structure.\n");
    return;
  }

  memset(new_info, 0, sizeof(libwebsock_close_info));
  memcpy(&code_be, close_frame->rawdata + close_frame->payload_offset, 2);
  at_most = close_frame->payload_len - 2;
  at_most = at_most > 124 ? 124 : at_most;
  new_info->code = ntohs(code_be);
  if (close_frame->payload_len - 2 > 0) {
    memcpy(new_info->reason, close_frame->rawdata + close_frame->payload_offset + 2, at_most);
  }
  *info = new_info;
}

void
libwebsock_shutdown(libwebsock_client_state *state)
{
  libwebsock_context *ctx = (libwebsock_context *) state->ctx;
  struct event *ev;
  struct timeval tv = { 1, 0 };
  if ((state->flags & STATE_CONNECTED) && state->onclose) {
    state->onclose(state);
  }
  bufferevent_free(state->bev);
  //schedule cleanup.
  ev = event_new(ctx->base, -1, 0, libwebsock_post_shutdown_cleanup, (void *) state);
  event_add(ev, &tv);
}

void
libwebsock_shutdown_after_send(struct bufferevent *bev, void *arg)
{
  struct event *ev;
  struct timeval tv = { 0, 0 };
  libwebsock_client_state *state = (libwebsock_client_state *) arg;
  libwebsock_context *ctx = state->ctx;
  ev = event_new(ctx->base, -1, 0, libwebsock_shutdown_after_send_cb, (void *) state);
  event_add(ev, &tv);
}

void
libwebsock_shutdown_after_send_cb(evutil_socket_t fd, short what, void *arg)
{
  libwebsock_client_state *state = (libwebsock_client_state *) arg;
  libwebsock_shutdown(state);
}

void
libwebsock_post_shutdown_cleanup(evutil_socket_t fd, short what, void *arg)
{
  libwebsock_client_state *state = (libwebsock_client_state *) arg;
  libwebsock_string *str;
  if (!state) {
    return;
  }
  libwebsock_free_all_frames(state);
  if (state->close_info) {
    free(state->close_info);
  }
  if (state->sa) {
    free(state->sa);
  }
  if (state->flags & STATE_CONNECTING) {
    if (state->data) {
      str = state->data;
      if (str->data) {
        free(str->data);
      }
      free(str);
    }
  }
  free(state);
}

void
libwebsock_handle_send(struct bufferevent *bev, void *arg)
{
}

void
libwebsock_send_cleanup(const void *data, size_t len, void *arg)
{
  free((void *) data);
}

int
libwebsock_send_fragment(libwebsock_client_state *state, const char *data, unsigned int len, int flags)
{
  struct evbuffer *output = bufferevent_get_output(state->bev);
  unsigned int *payload_len_32_be;
  unsigned short int *payload_len_short_be;
  unsigned char finNopcode, payload_len_small;
  unsigned int payload_offset = 2;
  unsigned int frame_size;
  char *frame;

  finNopcode = flags & 0xff;
  if (len <= 125) {
    frame_size = 2 + len;
    payload_len_small = len & 0xff;
  } else if (len > 125 && len <= 0xffff) {
    frame_size = 4 + len;
    payload_len_small = 126;
    payload_offset += 2;
  } else if (len > 0xffff && len <= 0xfffffff0) {
    frame_size = 10 + len;
    payload_len_small = 127;
    payload_offset += 8;
  } else {
    fprintf(stderr, "libwebsock does not support frame payload sizes over %u bytes long\n", 0xfffffff0);
    return -1;
  }
  frame = (char *) malloc(frame_size);
  payload_len_small &= 0x7f;
  *frame = finNopcode;
  *(frame + 1) = payload_len_small;
  if (payload_len_small == 126) {
    len &= 0xffff;
    payload_len_short_be = (unsigned short *) ((char *)frame + 2);
    *payload_len_short_be = htons(len);
  }
  if (payload_len_small == 127) {
    payload_len_32_be = (unsigned int *) ((char *)frame + 2);
    *payload_len_32_be++ = 0;
    *payload_len_32_be = htonl(len);
  }
  memcpy(frame + payload_offset, data, len);

  return evbuffer_add_reference(output, frame, frame_size, libwebsock_send_cleanup, NULL);
}

void
libwebsock_handle_accept(evutil_socket_t listener, short event, void *arg)
{
  libwebsock_context *ctx = arg;
  libwebsock_client_state *client_state;
  struct bufferevent *bev;
  struct sockaddr_storage ss;
  socklen_t slen = sizeof(ss);
  int fd = accept(listener, (struct sockaddr *) &ss, &slen);
  if (fd < 0) {
    fprintf(stderr, "Error accepting new connection.\n");
  } else {
    client_state = (libwebsock_client_state *) malloc(sizeof(libwebsock_client_state));
    if (!client_state) {
      fprintf(stderr, "Unable to allocate memory for new connection state structure.\n");
      close(fd);
      return;
    }
    memset(client_state, 0, sizeof(libwebsock_client_state));
    client_state->sockfd = fd;
    client_state->flags |= STATE_CONNECTING;
    client_state->control_callback = ctx->control_callback;
    client_state->onopen = ctx->onopen;
    client_state->onmessage = ctx->onmessage;
    client_state->onclose = ctx->onclose;
    client_state->sa = (struct sockaddr_storage *) malloc(sizeof(struct sockaddr_storage));
    if (!client_state->sa) {
      fprintf(stderr, "Unable to allocate memory for sockaddr_storage.\n");
      free(client_state);
      close(fd);
      return;
    }
    client_state->ctx = (void *) ctx;
    memcpy(client_state->sa, &ss, sizeof(struct sockaddr_storage));
    evutil_make_socket_nonblocking(fd);
    bev = bufferevent_socket_new(ctx->base, fd, BEV_OPT_CLOSE_ON_FREE);
    client_state->bev = bev;
    bufferevent_setcb(bev, libwebsock_handshake, libwebsock_handle_send, libwebsock_do_event, (void *) client_state);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
  }
}

void
libwebsock_do_event(struct bufferevent *bev, short event, void *ptr)
{
  libwebsock_client_state *state = ptr;

  if (event & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
    libwebsock_shutdown(state);
  }
}

void
libwebsock_handle_recv(struct bufferevent *bev, void *ptr)
{
  //alright... while we haven't reached the end of data keep trying to build frames
  //possible states right now:
  // 1.) we're receiving the beginning of a new frame
  // 2.) we're receiving more data from a frame that was created previously and was not complete
  libwebsock_client_state *state = ptr;
  libwebsock_frame *current = NULL;
  struct evbuffer *input;
  struct evbuffer_iovec *iovec, *iovec_mem;
  int i, datalen, err, n_vec, consumed, in_fragment;
  char *buf;
  void (*frame_fn)(libwebsock_client_state *state);

  input = bufferevent_get_input(bev);
  n_vec = evbuffer_peek(input, -1, NULL, NULL, 0);
  if (!n_vec) {
    fprintf(stderr, "Weird evbuffer_peek returned 0 vectors available\n");
    return;
  }
  iovec_mem = iovec = (struct evbuffer_iovec *) malloc(sizeof(struct evbuffer_iovec) * (n_vec + 1));
  if (!iovec_mem) {
    fprintf(stderr, "Unable to allocate memory for io vectors.\n");
    return;
  }
  iovec[n_vec].iov_base = NULL;
  evbuffer_peek(input, -1, NULL, iovec, n_vec);
  consumed = 0;
  while ((buf = iovec->iov_base) != NULL) {
    datalen = (iovec++)->iov_len;
    consumed += datalen;
    for (i = 0; i < datalen; ) {
      current = state->current_frame;
      if (current == NULL) {
        current = (libwebsock_frame *) malloc(sizeof(libwebsock_frame));
        memset(current, 0, sizeof(libwebsock_frame));
        current->payload_len = -1;
        current->rawdata_sz = FRAME_CHUNK_LENGTH;
        current->rawdata = (char *) malloc(FRAME_CHUNK_LENGTH);
        state->current_frame = current;
      }

      *(current->rawdata + current->rawdata_idx++) = *buf++;
      i++;

      if (current->state != sw_loaded_mask) {
        err = libwebsock_read_header(current);
        if (err == -1) {
          if ((state->flags & STATE_SENT_CLOSE_FRAME) == 0) {
            libwebsock_fail_connection(state, WS_CLOSE_PROTOCOL_ERROR);
            continue;
          }
        }
        if (err == 0) {
          continue;
        }
      }

      if (current->rawdata_idx < current->size) {
        if (datalen - i >= current->size - current->rawdata_idx) {  //remaining in current vector completes frame.  Copy remaining frame size
          memcpy(current->rawdata + current->rawdata_idx, buf, current->size - current->rawdata_idx);
          buf += current->size - current->rawdata_idx;
          i += current->size - current->rawdata_idx;
          current->rawdata_idx = current->size;
        } else { //not complete frame, copy the rest of this vector into frame.
          memcpy(current->rawdata + current->rawdata_idx, buf, datalen - i);
          current->rawdata_idx += datalen - i;
          i = datalen;
          continue;
        }
      }

      //have full frame at this point

      if (state->flags & STATE_FAILING_CONNECTION) {
        if (current->opcode != WS_OPCODE_CLOSE) {
          libwebsock_cleanup_frames(current);
          state->current_frame = NULL;
          continue;
        }
      }

      in_fragment = (state->flags & STATE_RECEIVING_FRAGMENT) ? 256 : 0;

      assert(state->current_frame == current);

      frame_fn = libwebsock_frame_lookup_table[in_fragment | (*current->rawdata & 0xff)];

      assert(frame_fn != NULL);

      frame_fn(state);
    }
  }
  evbuffer_drain(input, consumed);
  free(iovec_mem);
}

void
libwebsock_fail_connection(libwebsock_client_state *state, unsigned short close_code)
{
  struct evbuffer *output = bufferevent_get_output(state->bev);
  char close_frame[4] = { 0x88, 0x02, 0x00, 0x00 };

  unsigned short *code_be = (unsigned short *) &close_frame[2];

  if ((state->flags & STATE_FAILING_CONNECTION) != 0) {
    return;
  }
  *code_be = htobe16(close_code);

  evbuffer_add(output, close_frame, 4);
  state->flags |= STATE_SHOULD_CLOSE|STATE_SENT_CLOSE_FRAME|STATE_FAILING_CONNECTION;
}

void
libwebsock_dispatch_message(libwebsock_client_state *state)
{
  unsigned int current_payload_len;
  unsigned long long message_payload_len, message_offset;
  int message_opcode, i;
  libwebsock_frame *current = state->current_frame;
  char *message_payload, *message_payload_orig, *rawdata_ptr;

  state->flags &= ~STATE_RECEIVING_FRAGMENT;
  if (state->flags & STATE_SENT_CLOSE_FRAME) {
     return;
  }
  libwebsock_frame *first = NULL;
  if (current == NULL) {
    fprintf(stderr, "Somehow, null pointer passed to libwebsock_dispatch_message.\n");
    exit(1);
  }
  message_offset = 0;
  message_payload_len = 0;
  for (; current->prev_frame != NULL; current = current->prev_frame) {
    message_payload_len += current->payload_len;
  }
  message_payload_len += current->payload_len;
  first = current;
  message_opcode = current->opcode;
  message_payload = (char *) malloc(message_payload_len + 1);
  message_payload_orig = message_payload;

  for (; current != NULL; current = current->next_frame) {
    current_payload_len = current->payload_len;
    rawdata_ptr = current->rawdata + current->payload_offset;
    for (i = 0; i < current_payload_len; i++) {
      *message_payload++ = *rawdata_ptr++ ^ current->mask[i & 3];
    }
  }

  *(message_payload) = '\0';

  if(message_opcode == WS_OPCODE_TEXT) {
    if(!validate_utf8_sequence((uint8_t *)message_payload_orig)) {
      fprintf(stderr, "Error validating UTF-8 sequence.\n");
      free(message_payload_orig);
      libwebsock_fail_connection(state, WS_CLOSE_WRONG_TYPE);
      libwebsock_cleanup_frames(first);
      state->current_frame = NULL;
      return;
    }
  }

  libwebsock_cleanup_frames(first);
  state->current_frame = NULL;

  libwebsock_message msg = { .opcode = message_opcode, .payload_len = message_payload_len, .payload = message_payload_orig };
  if (state->onmessage != NULL) {
    state->onmessage(state, &msg);
  } else {
    fprintf(stderr, "No onmessage call back registered with libwebsock.\n");
  }
  free(message_payload_orig);
}

void
libwebsock_handshake_finish(struct bufferevent *bev, libwebsock_client_state *state)
{
  //TODO: this is shite.  Clean it up.
  libwebsock_string *str = state->data;
  struct evbuffer *output;
  char buf[1024];
  char sha1buf[45];
  char concat[1024];
  unsigned char sha1mac[20];
  char *tok = NULL, *headers = NULL, *key = NULL;
  char *base64buf = NULL;
  const char *GID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  SHA1Context shactx;
  SHA1Reset(&shactx);
  int n = 0;

  output = bufferevent_get_output(bev);

  headers = (char *) malloc(str->data_sz + 1);
  if (!headers) {
    fprintf(stderr, "Unable to allocate memory in libwebsock_handshake..\n");
    bufferevent_free(bev);
    return;
  }
  memset(headers, 0, str->data_sz + 1);
  strncpy(headers, str->data, str->idx);
  for (tok = strtok(headers, "\r\n"); tok != NULL; tok = strtok(NULL, "\r\n")) {
    if (strstr(tok, "Sec-WebSocket-Key: ") != NULL) {
      key = (char *) malloc(strlen(tok));
      strncpy(key, tok + strlen("Sec-WebSocket-Key: "), strlen(tok));
      break;
    }
  }
  free(headers);
  free(str->data);
  free(str);
  state->data = NULL;

  if (key == NULL) {
    fprintf(stderr, "Unable to find key in request headers.\n");
    bufferevent_free(bev);
    return;
  }

  memset(concat, 0, sizeof(concat));
  strncat(concat, key, strlen(key));
  strncat(concat, GID, strlen(GID));
  SHA1Input(&shactx, (unsigned char *) concat, strlen(concat));
  SHA1Result(&shactx);
  free(key);
  key = NULL;
  sprintf(sha1buf, "%08x%08x%08x%08x%08x", shactx.Message_Digest[0], shactx.Message_Digest[1], shactx.Message_Digest[2],
      shactx.Message_Digest[3], shactx.Message_Digest[4]);
  for (n = 0; n < (strlen(sha1buf) / 2); n++) {
    sscanf(sha1buf + (n * 2), "%02hhx", sha1mac + n);
  }
  base64buf = (char *) malloc(256);
  base64_encode(sha1mac, 20, base64buf, 256);
  memset(buf, 0, 1024);
  snprintf(buf, 1024,
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Server: %s/%s\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: %s\r\n\r\n", PACKAGE_NAME, PACKAGE_VERSION, base64buf);
  free(base64buf);

  evbuffer_add(output, buf, strlen(buf));
  bufferevent_setcb(bev, libwebsock_handle_recv, libwebsock_handle_send, libwebsock_do_event, (void *) state);

  state->flags &= ~STATE_CONNECTING;
  state->flags |= STATE_CONNECTED;

  if (state->onopen != NULL) {
    state->onopen(state);
  }
}

void
libwebsock_handshake(struct bufferevent *bev, void *ptr)
{
  //TODO: this is shite too.
  libwebsock_client_state *state = ptr;
  libwebsock_string *str = NULL;
  struct evbuffer *input;
  char buf[1024];
  int datalen;
  input = bufferevent_get_input(bev);
  str = state->data;
  if (!str) {
    state->data = (libwebsock_string *) malloc(sizeof(libwebsock_string));
    if (!state->data) {
      fprintf(stderr, "Unable to allocate memory in libwebsock_handshake.\n");
      bufferevent_free(bev);
      return;
    }
    str = state->data;
    memset(str, 0, sizeof(libwebsock_string));
    str->data_sz = FRAME_CHUNK_LENGTH;
    str->data = (char *) malloc(str->data_sz);
    if (!str->data) {
      fprintf(stderr, "Unable to allocate memory in libwebsock_handshake.\n");
      bufferevent_free(bev);
      return;
    }
    memset(str->data, 0, str->data_sz);
  }

  while (evbuffer_get_length(input)) {
    datalen = evbuffer_remove(input, buf, sizeof(buf));

    if (str->idx + datalen >= str->data_sz) {
      str->data = realloc(str->data, str->data_sz * 2 + datalen);
      if (!str->data) {
        fprintf(stderr, "Failed realloc.\n");
        bufferevent_free(bev);
        return;
      }
      str->data_sz += str->data_sz + datalen;
      memset(str->data + str->idx, 0, str->data_sz - str->idx);
    }
    memcpy(str->data + str->idx, buf, datalen);
    str->idx += datalen;
    if (strstr(str->data, "\r\n\r\n") != NULL || strstr(str->data, "\n\n") != NULL) {
      libwebsock_handshake_finish(bev, state);
    }
  }
}


