#pragma once

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <unistd.h>

#include "uv.h"
#include "http_parser.h"

using namespace std;

static uv_loop_t* uv_loop;

#if defined(NDEBUG)
# define ASSERT(exp)
#else
# define ASSERT(exp)  assert(exp)
#endif

// FIXME : to stderr with __LINE__, __PRETTY_FUNCTION__ ?
#define LOGE(...) LOGI(__VA_ARGS__)
void _LOGI(){}
template <typename T, typename ...Args>
void _LOGI(T t, Args && ...args)
{
    if (getenv("DEBUG_LOG")) {
    try {
      std::ofstream logFile;
      logFile.open(getenv("DEBUG_LOG"), std::ios::out | std::ios::app);
      logFile << std::forward<T>(t);
      logFile.close();
    } catch(const std::exception &e) {
      std::cerr << e.what() << std::endl;
    }
  } else
    std::cout << std::forward<T>(t);

  _LOGI(std::forward<Args>(args)...);
}
template <typename T, typename ...Args>
void LOGI(T t, Args && ...args)
{
  if (!getenv("DEBUG")) return;

  _LOGI("[" + ::to_string(getpid()) + "] ");  // insert pid
  _LOGI(std::forward<T>(t));
  _LOGI(std::forward<Args>(args)...);
  _LOGI('\n');
}


template<class... T>
using Callback = std::map<std::string, std::function<void(T...)>>;

class SimpleHttpRequest {
 public:
  SimpleHttpRequest(map<string, string> &options, uv_loop_t *loop) :  SimpleHttpRequest(loop) {
   this->options = options;
  }
  SimpleHttpRequest(map<string, string> &options, map<string, string> &requestHeaders, uv_loop_t *loop) :  SimpleHttpRequest(loop) {
   this->options = options;
   this->requestHeaders = requestHeaders;
  }
  SimpleHttpRequest(uv_loop_t *loop) : uv_loop(loop) {
    allocCb = [](uv_handle_t* handle, size_t size, uv_buf_t* buf) {
      buf->base = (char*)malloc(size);
      buf->len = size;
    };

    onClose = [](uv_handle_t* handle) {
      SimpleHttpRequest *client = (SimpleHttpRequest*)handle->data;
      LOGI("onClose");
      handle->data = NULL;
      uv_close((uv_handle_t*)&client->timer, [](uv_handle_t*){});
    };

    http_parser_init(&parser, HTTP_RESPONSE);
    http_parser_settings_init(&parser_settings);
    parser_settings.on_message_begin = [](http_parser* parser) {
      return 0;
    };
    parser_settings.on_url = [](http_parser *p, const char *buf, size_t len) {
      LOGI("Url: ", string(buf,len));
      return 0;
    };

    parser_settings.on_header_field = [](http_parser *p, const char *buf, size_t len) {
      LOGI("Header field: ", string(buf,len));
      SimpleHttpRequest *client = (SimpleHttpRequest*)p->data;
      client->lastHeaderFieldBuf = (char*)buf;
      client->lastHeaderFieldLenth = (int)len;
      return 0;
    };
    parser_settings.on_header_value = [](http_parser *p, const char* buf, size_t len) {
      LOGI("Header value: ", string(buf,len));

      SimpleHttpRequest *client = (SimpleHttpRequest*)p->data;

      //on_header_value called without on_header_field
      if (client->lastHeaderFieldBuf == NULL) {
        LOGI("broken header field.");
        return 0;
      }

      string field = string(client->lastHeaderFieldBuf, client->lastHeaderFieldLenth);
      string value = string(buf, len);

      transform(field.begin(), field.end(), field.begin(), ::tolower);
      client->responseHeaders[field] = value;

      client->lastHeaderFieldBuf = NULL;
      return 0;
    };
    parser_settings.on_headers_complete = [](http_parser *p) {
      LOGI("on_headers_complete");

      SimpleHttpRequest *client = (SimpleHttpRequest*)p->data;
      return 0;
    };

    parser_settings.on_body = [](http_parser* parser, const char* buf, size_t len) {
      SimpleHttpRequest *client = (SimpleHttpRequest*)parser->data;
      if (buf)
        client->responseBody.rdbuf()->sputn(buf, len);

      //fprintf("Body: %.*s\n", (int)length, at);
      if (http_body_is_final(parser)) {
          LOGI("http_body_is_final");
      } else {
      }

      return 0;
    };

    parser_settings.on_message_complete = [](http_parser* parser) {
      LOGI("on_message_complete");
      SimpleHttpRequest *client = (SimpleHttpRequest*)parser->data;
      ssize_t total_len = client->responseBody.str().size();
      LOGI("total_len: ",total_len);
      if (http_should_keep_alive(parser)) {
          LOGI("http_should_keep_alive");
          uv_stream_t* tcp = (uv_stream_t*)&client->tcp;
          uv_close((uv_handle_t*)tcp, client->onClose);
      }
      LOGI("status code : ", ::to_string(parser->status_code));

      // response should be called after on_message_complete
      //LOGI(client->responseBody.str().c_str());
      client->statusCode = parser->status_code;
      client->emit("response");

      return 0;
    };
  }
  ~SimpleHttpRequest() {}

  //FIXME : variadic ..Args
  SimpleHttpRequest& emit(string name) {
    if (eventListeners.count(name))
      eventListeners[name]();

    return *this;
  }

  template <class... Args>
  SimpleHttpRequest& on(string name, std::function<void()> func) {
    eventListeners[name] = func;

    return *this;
  }
  SimpleHttpRequest& on(string name, std::function<void()> func) {
    eventListeners[name] = func;

    return *this;
  }

  //FIXME : send directly later when tcp is open
  SimpleHttpRequest& write(const std::istream &stream) {
    this->requestHeaders["content-type"] = "application/octet-stream";
    requestBody << stream.rdbuf();

    return *this;
  }
  SimpleHttpRequest& write(const char* s, std::streamsize n) {
    requestBody.rdbuf()->sputn(s, n);
    return *this;
  }
  SimpleHttpRequest& write(const string &data) {
    requestBody << data;

    return *this;
  }

  SimpleHttpRequest& setHeader(const string &field, const string &value) {
    this->requestHeaders[field] = value;

    return *this;
  }

  // GET/HEAD verb
  SimpleHttpRequest& get(const string &url) {
    return _get(url, "GET");
  }
  SimpleHttpRequest& head(const string &url) {
    return _get(url, "HEAD");
  }

  // POST/PUT verbs with payloads. 'DELETE' is reserved in c. so, use 'del()'
  SimpleHttpRequest& post(const string &url, const string &body) {
    return _post(url, "POST", body);
  }
  SimpleHttpRequest& put(const string &url, const string &body) {
    return _post(url, "PUT", body);
  }
  SimpleHttpRequest& del(const string &url, const string &body) {
    return _post(url, "DELETE", body);
  }

  void end() {
    int r = uv_ip4_addr(options["hostname"].c_str(), stoi(options["port"]), &addr);
    if (r != 0) {
      LOGE("uv_ip4_addr", uv_err_name(r), uv_strerror(r));
      this->emit("error");
      return;
    }

    r = uv_tcp_init(uv_loop, &tcp);
    if (r != 0) {
      LOGE("uv_tcp_init", uv_err_name(r), uv_strerror(r));
      this->emit("error");
      return;
    }

    tcp.data = this;          //FIXME : use one
    connect_req.data = this;
    parser.data = this;
    write_req.data = this;
    timer.data = this;

    r = uv_timer_init(uv_loop, &timer);
    ASSERT(r == 0);

    r = uv_timer_start(&timer, [](uv_timer_t* timer){
      SimpleHttpRequest *client = (SimpleHttpRequest*)timer->data;
      uv_close((uv_handle_t*)timer, [](uv_handle_t*){});
      uv_close((uv_handle_t*)&client->tcp, [](uv_handle_t*){});

      client->emit("error");
    }, timeout, 0);
    ASSERT(r == 0);

    r = uv_tcp_connect(&connect_req, &tcp, reinterpret_cast<const sockaddr*>(&addr),
      [](uv_connect_t *req, int status) {
        SimpleHttpRequest *client = (SimpleHttpRequest*)req->data;
        if (status != 0) {
            uv_close((uv_handle_t*)req->handle, client->onClose);
            LOGE("uv_connect_cb", uv_err_name(status), uv_strerror(status));
            client->emit("error");
            return;
        }

        LOGI("TC connection established to ",
          client->options["hostname"].c_str(),
          ":",client->options["port"].c_str());

        int r = uv_read_start(req->handle, client->allocCb,
          [](uv_stream_t *tcp, ssize_t nread, const uv_buf_t * buf) {
            ssize_t parsed;
            SimpleHttpRequest* client = (SimpleHttpRequest*)tcp->data;
            LOGI("onRead ", nread);
            LOGI("buf len: ", buf->len);
            if (nread > 0) {
              http_parser *parser = &client->parser;
              parsed = (ssize_t)http_parser_execute(parser, &client->parser_settings, buf->base, nread);

              LOGI("parsed: ", parsed);
              if (parser->upgrade) {
                LOGE("raise upgrade unsupported");
                client->emit("error");
                return;
              } else if (parsed != nread) {
                LOGE("http parse error. ", parsed," parsed of", nread);
                LOGE(http_errno_description(HTTP_PARSER_ERRNO(parser)));
                client->emit("error");
                return;
              }
            } else {
              if (nread != UV_EOF) {
                LOGE("uv_read_cb", uv_err_name(nread), uv_strerror(nread));
                client->emit("error");
                return;
              }
            }

            //LOGI(buf->base);

            free(buf->base);
        });

        if (r != 0) {
          LOGE("uv_read_start", uv_err_name(r), uv_strerror(r));
          client->emit("error");
          return;
        }

        uv_buf_t resbuf;
        string res = client->options["method"] + " " + client->options["path"] + " " + "HTTP/1.1\r\n";
        for (const auto &kv : client->requestHeaders) {
          res += kv.first + ":" + kv.second + "\r\n";
        }
        if (client->requestBody.str().size() > 0) {
          res += "Content-Length: " + ::to_string(client->requestBody.str().size()) + "\r\n";
          res += "\r\n";
          res += client->requestBody.str();
        } else {
          res += "\r\n";
        }
        resbuf.base = (char *)res.c_str();
        resbuf.len = res.size();

        r = uv_write(&client->write_req, req->handle, &resbuf, 1,
          [](uv_write_t* req, int status) {
            SimpleHttpRequest* client = (SimpleHttpRequest*)req->data;
            LOGI("uv_write");
            if (status != 0) {
              LOGE("uv_write_cb", uv_err_name(status), uv_strerror(status));
              client->emit("error");
              return;
            }
        });

        if (r != 0) {
          LOGE("uv_write", uv_err_name(r), uv_strerror(r));
          client->emit("error");
          return;
        }
    });

    if (r != 0) {
      LOGE("uv_tcp_connect", uv_err_name(r), uv_strerror(r));
      this->emit("error");
      return;
    }
  }

  map<string, string> responseHeaders;
  stringstream responseBody;
  unsigned int statusCode = 0;
  unsigned int timeout = 60*2*1000; // 2 minutes default.
 private:
  uv_loop_t* uv_loop;

  sockaddr_in addr;
  uv_tcp_t tcp;
  uv_connect_t connect_req;
  uv_write_t write_req;
  uv_timer_t timer;

  http_parser_settings parser_settings;
  http_parser parser;
  stringstream requestBody;

  uv_alloc_cb allocCb;
  uv_close_cb onClose;

  Callback<> eventListeners;

  map<string, string> options;
  map<string, string> requestHeaders;

  char* lastHeaderFieldBuf;
  int lastHeaderFieldLenth;

  SimpleHttpRequest& _get(const string &url, const string &method) {
    options["method"] = method;
    if(!_parseUrl(url)) {
      //FIXME : logic error. on("error") may be next. USE uv_loop!!
      this->emit("error");
    }
    return *this;
  }
  SimpleHttpRequest& _post(const string &url, const string &method, const string& body) {
    options["method"] = method;
    if(!_parseUrl(url)) {
      this->emit("error");  //FIXME : logic error.
    }
    this->write(body);
    return *this;
  }

  bool _parseUrl(const string &url) {
    struct http_parser_url u;
    http_parser_url_init(&u);

    if (http_parser_parse_url(url.c_str(), url.length(), 0, &u)) {
      LOGE("failed to parse URL - ", url);
      return false;
    }

    options["hostname"] = url.substr(u.field_data[UF_HOST].off, u.field_data[UF_HOST].len);
    options["port"] = url.substr(u.field_data[UF_PORT].off, u.field_data[UF_PORT].len);
    options["path"] = url.substr(u.field_data[UF_PATH].off, u.field_data[UF_PATH].len);

    if(options["port"].length() == 0) {
      auto protocol = url.substr(u.field_data[UF_SCHEMA].off, u.field_data[UF_SCHEMA].len);

      // FIXME : well-known protocols
      if (protocol.compare("http") == 0) {
        options["port"] = "80";
      } else if (protocol.compare("https") == 0) {
        options["port"] = "443";
      } else {
        options["port"] = "0";
      }
    }

    return true;
  }
};