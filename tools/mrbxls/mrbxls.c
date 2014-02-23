#include "mruby.h"
#include "mruby/proc.h"
#include "mruby/array.h"
#include "mruby/string.h"
#include "mruby/compile.h"
#include "mruby/dump.h"
#include "mruby/variable.h"
#include "windows.h"
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mrb_state* smrb;

void
sock_send(mrb_state *mrb, SOCKET sock, char *verb, char *path)
{
  char *buf;
  int buf_len = 256;
  buf = (char *)mrb_malloc(mrb, sizeof(char) * buf_len);
  char *ver  = "HTTP/1.0";
  memset(buf, 0, buf_len);
  _snprintf(buf, buf_len, "%s %s %s\r\n\r\n", verb, path, ver);

  int n = send(sock, buf, (int)strlen(buf), 0);

  if (n < 0) {
    char err[255];
    _snprintf(err, sizeof(buf), "send : %d", WSAGetLastError());
    mrb_raise(mrb, E_ARGUMENT_ERROR, err);
  }

  free(buf);
}


mrb_value
sock_recv(mrb_state *mrb, SOCKET sock)
{
  mrb_value s1;
  mrb_value s2 = mrb_str_new(mrb, 0, 0);

  char *tmp;
  int n, len = 256;
  tmp = (char *)mrb_malloc(mrb, sizeof(char) * len);

  n = 1;
  while (n > 0) {
    memset(tmp, 0, len);
    n = recv(sock, tmp, len, 0);

    if (n < 0) {
      char err[255];
      _snprintf(err, sizeof(err), "recv : %d", WSAGetLastError());
      mrb_raise(mrb, E_ARGUMENT_ERROR, err);
    }

    s1 = mrb_str_new(mrb, tmp, n);
    s2 = mrb_str_plus(mrb, s2, s1);
  }

  free(tmp);

  return s2;
}

void
sock_init(mrb_state *mrb, SOCKET *sock, char *deststr)
{
  WSADATA wsaData;
  unsigned int **addrptr;

  if(WSAStartup ( MAKEWORD (2, 0), &wsaData ) != 0)
    mrb_raise(mrb, E_ARGUMENT_ERROR, "WSAStartup failed.");

  *sock = socket(AF_INET, SOCK_STREAM, 0);
  if (*sock == INVALID_SOCKET) {
    char buf[255];
    _snprintf(buf, sizeof(buf), "socket : %d", WSAGetLastError());
    mrb_raise(mrb, E_ARGUMENT_ERROR, buf);
  }

  struct sockaddr_in server;
  server.sin_family = AF_INET;
  server.sin_port = htons(80);

  server.sin_addr.S_un.S_addr = inet_addr(deststr);
  if (server.sin_addr.S_un.S_addr == 0xffffffff) {
    struct hostent *host;
    
    host = gethostbyname(deststr);
    if (host == NULL) {
      char buf[255];

      if (WSAGetLastError() == WSAHOST_NOT_FOUND)
        _snprintf(buf, sizeof(buf), "host not found : %s", deststr);
      else
        _snprintf(buf, sizeof(buf), "host not found.");

      mrb_raise(mrb, E_ARGUMENT_ERROR, buf);
    }

    addrptr = (unsigned int **)host->h_addr_list;

    while (*addrptr != NULL) {
      server.sin_addr.S_un.S_addr = *(*addrptr);

      if (connect(*sock,
            (struct sockaddr *)&server,
            sizeof(server)) == 0) {
        break;
      }

      addrptr++;
    }

    if (*addrptr == NULL) {
      char buf[255];
      _snprintf(buf, sizeof(buf), "connect : %d", WSAGetLastError());
      mrb_raise(mrb, E_ARGUMENT_ERROR, buf);
    }
  }
  else {
    if (connect(
          *sock,
          (struct sockaddr *)&server,
          sizeof(server)) != 0) {
      char buf[255];
      _snprintf(buf, sizeof(buf), "connect : %d", WSAGetLastError());
      mrb_raise(mrb, E_ARGUMENT_ERROR, buf);
    }
  }
}

mrb_value
mrb_sock_request(mrb_state *mrb, mrb_value self)
{
  mrb_value s;
  char *addr, *verb, *path;
  SOCKET sock;

  mrb_get_args(mrb, "zzz", &addr, &verb, &path);

  sock_init(mrb, &sock, addr);

  sock_send(mrb, sock, verb, path);

  s = sock_recv(mrb, sock);

  WSACleanup();

  return s;
}

int
conv_enc(mrb_state *mrb, char *buf_src, char *buf_dst, UINT src_enc, UINT dst_enc)
{
  const int n = MultiByteToWideChar(
      src_enc,
      0,
      buf_src,
      -1,
      NULL,
      0);

  wchar_t *buf_UTF16 = (wchar_t *)mrb_malloc(mrb, sizeof(wchar_t) * (n + 1));
  memset(buf_UTF16, 0, n + 1);

  MultiByteToWideChar(
      src_enc,
      0,
      buf_src,
      -1,
      (LPWSTR)buf_UTF16,
      n + 1);

  int m = WideCharToMultiByte(
      dst_enc,
      0,
      buf_UTF16,
      -1,
      NULL,
      0,
      NULL,
      NULL);

  if (!buf_dst)
  {
    free(buf_UTF16);
    return m;
  }

  memset(buf_dst, 0, m);

  m = WideCharToMultiByte(
      dst_enc,
      0,
      buf_UTF16,
      -1,
      buf_dst,
      m, NULL, NULL);

  free(buf_UTF16);

  return m;
}

mrb_value
mrb_cstr_to_utf8(mrb_state *mrb, mrb_value self)
{
  char *buf_SJIS = RSTRING_PTR(self);
  int size = conv_enc(mrb, buf_SJIS, NULL, CP_ACP, CP_UTF8);
  char *buf_UTF8 = (char *)mrb_malloc(mrb, sizeof(char) * (size));
  memset(buf_UTF8, 0, size);
  size = conv_enc(mrb, buf_SJIS, buf_UTF8, CP_ACP, CP_UTF8);
  mrb_value str = mrb_str_new(mrb, buf_UTF8, size);

  free(buf_UTF8);
  return str;
}

char *
file_read(mrb_state *mrb, char* fname)
{
  FILE* fp;
  long size;
  char *buf;

  if (NULL == (fp = fopen(fname, "rb+"))) {
    char err[300];
    _snprintf(err, sizeof(err), "file is not found : %s", fname);
    mrb_raise(mrb, E_ARGUMENT_ERROR, err);
  }

  fseek(fp, 0, SEEK_END);

  size = ftell(fp);

  fseek(fp, 0, SEEK_SET);

  buf = (char *)mrb_malloc(mrb, (size + 1) * sizeof(char));
  memset(buf, 0, size+1);

  fread(buf, sizeof(char), size, fp);

  fclose(fp);

  return buf;
}


mrb_value
mrb_file_read(mrb_state *mrb, mrb_value self)
{
  char* fname;
  mrb_get_args(mrb, "z", &fname);

  char *buf = file_read(mrb, fname);;
  mrb_value str = mrb_str_new(mrb, buf, strlen(buf) + 1);
  free(buf);

  return str;
}

mrb_value
mrb_data_data(mrb_state *mrb, mrb_value self)
{
  mrb_value v;
  v = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@data"));
  if(mrb_nil_p(v)) {
    mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@data"), mrb_ary_new(mrb));
    v = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@data"));
  }
  return v;
}

int
__stdcall xls_mrb_open()
{
  smrb = mrb_open();
  mrb_state *mrb = smrb;
	if (mrb == NULL) {
		printf("mrb open error!!");
		return 1;
	}

  struct RClass* data = mrb_define_module(mrb, "XlsData");
  mrb_define_class_method(mrb, data, "data", mrb_data_data, ARGS_NONE());

  struct RClass* file = mrb_define_module(mrb, "TinyFile");
  mrb_define_class_method(mrb, file, "read", mrb_file_read, ARGS_OPT(1));

  struct RClass* sock = mrb_define_module(mrb, "TCPEasySocket");
  mrb_define_class_method(mrb, sock, "request", mrb_sock_request, ARGS_OPT(3));

  mrb_define_method(mrb, mrb->string_class, "sjis_to_utf8", mrb_cstr_to_utf8, ARGS_NONE());

  return mrb;
}

int
__stdcall xls_mrb_close()
{
  mrb_close(smrb);
  return 0;
}

mrb_value
get_data()
{
  return mrb_funcall(smrb, mrb_obj_value(mrb_module_get(smrb, "XlsData")), "data", 0);
}

int
__stdcall xls_get_ary_size_y()
{
  mrb_value a = get_data();

  return RARRAY_LEN(a);
}

int
__stdcall xls_get_ary_size_x(int y)
{
  mrb_value a = get_data();

  if(RARRAY_LEN(a) <= y)
    return 0;

  return RARRAY_LEN(RARRAY_PTR(a)[y]);
}

int
__stdcall xls_get_ary_data(char* str, int x, int y)
{
  mrb_value a = get_data();

  if(RARRAY_LEN(a) <= y)
    return -1;

  a = RARRAY_PTR(a)[y];

  if(mrb_nil_p(a))
    return -1;

  if(RARRAY_LEN(a) <= x)
    return -1;

  a = RARRAY_PTR(a)[x];

  if(mrb_nil_p(a)) {
    a = mrb_str_new(smrb, 0, 0);
  }
  else {
    if(mrb_type(a) != MRB_TT_STRING) {
      a = mrb_funcall(smrb, a, "inspect", 0);
    }
  }

  if (!str) {
    return RSTRING_LEN(a) + 1;
  }

  char *buf_UTF8 = mrb_str_to_cstr(smrb, a);
  int size = conv_enc(smrb, buf_UTF8, NULL, CP_UTF8, CP_ACP);
  char *buf_SJIS = (char *)mrb_malloc(smrb, sizeof(char) * size);
  memset(buf_SJIS, 0, size);
  size = conv_enc(smrb, buf_UTF8, buf_SJIS, CP_UTF8, CP_ACP);

  strcpy(str, buf_SJIS);

  free(buf_SJIS);
  buf_UTF8 = NULL;

  return size;
}


int
__stdcall xls_get_elem_size(int x, int y)
{
  return xls_get_ary_data(NULL, x, y);
}


int
__stdcall xls_load_mrb_file(char *fname)
{
	FILE *rfp;
  mrb_state *mrb = smrb;

	if (NULL == (rfp = fopen(fname, "r"))) {
		printf("file open error!!\n");
		return 1;
	}

  mrbc_context *cxt = mrbc_context_new(mrb);
  mrbc_filename(mrb, cxt, fname);
  mrb_load_file_cxt(mrb, rfp, cxt);
  mrbc_context_free(mrb, cxt);
	fclose(rfp);

	if (mrb->exc) {
    mrb_value s;
		s = mrb_funcall(mrb, mrb_obj_value(mrb->exc), "inspect", 0);
		printf("mrb_run failed:\n");
    if (mrb_string_p(s)) {
      struct RString *e_str = mrb_str_ptr(s);
      char *mb_str = (char *)mrb_malloc(mrb, sizeof(char) * (e_str->len + 1));
      memcpy(mb_str, e_str->ptr, e_str->len);
      mb_str[e_str->len] = '\0';
      printf(mb_str);
    }
		return 1;
	}

	return 0;
}

int
main(int argc, char *argv[])
{

  if (argc < 2) {
    printf("please set mruby file.\n");
    return -1;
  }

  printf("please any key to start.");
  getchar();

  for (int j = 0; j < 100; j++) {
    xls_mrb_open();

    xls_load_mrb_file(argv[1]);

    char *str;
    int size;
    for (int y = 0; y < xls_get_ary_size_y(); y++) {
      for (int x = 0; x < xls_get_ary_size_x(y); x++) {
        size = xls_get_ary_data(NULL, x, y);
        str = (char *)mrb_malloc(smrb, size * sizeof(char));
        memset(str, 0, size);
        xls_get_ary_data(str, x, y);
        // printf("%s|", str);
        free(str);
      }
      printf("\n");
    }

    xls_mrb_close();
  }

  printf("end.");
  getchar();

  return 0;
}
