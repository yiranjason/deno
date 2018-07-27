// Copyright 2018 the Deno authors. All rights reserved. MIT license.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "third_party/v8/include/libplatform/libplatform.h"
#include "third_party/v8/include/v8.h"
#include "third_party/v8/src/base/logging.h"

#include "deno.h"
#include "internal.h"

namespace deno {

static bool skip_onerror = false;

Deno* FromIsolate(v8::Isolate* isolate) {
  return static_cast<Deno*>(isolate->GetData(0));
}

// Extracts a C string from a v8::V8 Utf8Value.
const char* ToCString(const v8::String::Utf8Value& value) {
  return *value ? *value : "<string conversion failed>";
}

static inline v8::Local<v8::String> v8_str(const char* x) {
  return v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), x,
                                 v8::NewStringType::kNormal)
      .ToLocalChecked();
}

void HandleExceptionStr(v8::Local<v8::Context> context,
                        v8::Local<v8::Value> exception,
                        std::string* exception_str) {
  auto* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context);

  auto message = v8::Exception::CreateMessage(isolate, exception);
  auto onerrorStr = v8::String::NewFromUtf8(isolate, "onerror");
  auto onerror = context->Global()->Get(onerrorStr);
  auto stack_trace = message->GetStackTrace();
  auto line =
      v8::Integer::New(isolate, message->GetLineNumber(context).FromJust());
  auto column =
      v8::Integer::New(isolate, message->GetStartColumn(context).FromJust());

  if (skip_onerror == false) {
    if (onerror->IsFunction()) {
      // window.onerror is set so we try to handle the exception in javascript.
      auto func = v8::Local<v8::Function>::Cast(onerror);
      v8::Local<v8::Value> args[5];
      args[0] = exception->ToString();
      args[1] = message->GetScriptResourceName();
      args[2] = line;
      args[3] = column;
      args[4] = exception;
      func->Call(context->Global(), 5, args);
      /* message, source, lineno, colno, error */
    }
  }

  char buf[12 * 1024];
  if (!stack_trace.IsEmpty()) {
    // No javascript onerror handler, but we do have a stack trace. Format it
    // into a string and add to last_exception.
    std::string msg;
    v8::String::Utf8Value exceptionStr(isolate, exception);
    msg += ToCString(exceptionStr);
    msg += "\n";

    for (int i = 0; i < stack_trace->GetFrameCount(); ++i) {
      auto frame = stack_trace->GetFrame(i);
      v8::String::Utf8Value script_name(isolate, frame->GetScriptName());
      int l = frame->GetLineNumber();
      int c = frame->GetColumn();
      snprintf(buf, sizeof(buf), "%s %d:%d\n", ToCString(script_name), l, c);
      msg += buf;
    }
    *exception_str += msg;
  } else {
    // No javascript onerror handler, no stack trace. Format the little info we
    // have into a string and add to last_exception.
    v8::String::Utf8Value exceptionStr(isolate, exception);
    v8::String::Utf8Value script_name(isolate,
                                      message->GetScriptResourceName());
    v8::String::Utf8Value line_str(isolate, line);
    v8::String::Utf8Value col_str(isolate, column);
    snprintf(buf, sizeof(buf), "%s\n%s %s:%s\n", ToCString(exceptionStr),
             ToCString(script_name), ToCString(line_str), ToCString(col_str));
    *exception_str += buf;
  }
}

void HandleException(v8::Local<v8::Context> context,
                     v8::Local<v8::Value> exception) {
  v8::Isolate* isolate = context->GetIsolate();
  Deno* d = FromIsolate(isolate);
  std::string exception_str;
  HandleExceptionStr(context, exception, &exception_str);
  if (d != nullptr) {
    d->last_exception = exception_str;
  } else {
    printf("Pre-Deno Exception %s\n", exception_str.c_str());
  }
}

/*
bool AbortOnUncaughtExceptionCallback(v8::Isolate* isolate) {
  return true;
}

void MessageCallback2(Local<Message> message, v8::Local<v8::Value> data) {
  printf("MessageCallback2\n\n");
}

void FatalErrorCallback2(const char* location, const char* message) {
  printf("FatalErrorCallback2\n");
}
*/

void ExitOnPromiseRejectCallback(
    v8::PromiseRejectMessage promise_reject_message) {
  auto* isolate = v8::Isolate::GetCurrent();
  Deno* d = static_cast<Deno*>(isolate->GetData(0));
  DCHECK_EQ(d->isolate, isolate);
  v8::HandleScope handle_scope(d->isolate);
  auto exception = promise_reject_message.GetValue();
  auto context = d->context.Get(d->isolate);
  HandleException(context, exception);
}

void Print(const v8::FunctionCallbackInfo<v8::Value>& args) {
  CHECK_EQ(args.Length(), 1);
  auto* isolate = args.GetIsolate();
  v8::HandleScope handle_scope(isolate);
  v8::String::Utf8Value str(isolate, args[0]);
  const char* cstr = ToCString(str);
  printf("%s\n", cstr);
  fflush(stdout);
}

static v8::Local<v8::Uint8Array> ImportBuf(v8::Isolate* isolate, deno_buf buf) {
  if (buf.alloc_ptr == nullptr) {
    // If alloc_ptr isn't set, we memcpy.
    // This is currently used for flatbuffers created in Rust.
    auto ab = v8::ArrayBuffer::New(isolate, buf.data_len);
    memcpy(ab->GetContents().Data(), buf.data_ptr, buf.data_len);
    auto view = v8::Uint8Array::New(ab, 0, buf.data_len);
    return view;
  } else {
    auto ab = v8::ArrayBuffer::New(
        isolate, reinterpret_cast<void*>(buf.alloc_ptr), buf.alloc_len,
        v8::ArrayBufferCreationMode::kInternalized);
    auto view =
        v8::Uint8Array::New(ab, buf.data_ptr - buf.alloc_ptr, buf.data_len);
    return view;
  }
}

static deno_buf ExportBuf(v8::Isolate* isolate,
                          v8::Local<v8::ArrayBufferView> view) {
  auto ab = view->Buffer();
  auto contents = ab->Externalize();

  deno_buf buf;
  buf.alloc_ptr = reinterpret_cast<uint8_t*>(contents.Data());
  buf.alloc_len = contents.ByteLength();
  buf.data_ptr = buf.alloc_ptr + view->ByteOffset();
  buf.data_len = view->ByteLength();

  // Prevent JS from modifying buffer contents after exporting.
  ab->Neuter();

  return buf;
}

static void FreeBuf(deno_buf buf) { free(buf.alloc_ptr); }

// Sets the recv callback.
void Recv(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  Deno* d = reinterpret_cast<Deno*>(isolate->GetData(0));
  DCHECK_EQ(d->isolate, isolate);

  v8::HandleScope handle_scope(isolate);

  if (!d->recv.IsEmpty()) {
    isolate->ThrowException(v8_str("deno.recv already called."));
    return;
  }

  v8::Local<v8::Value> v = args[0];
  CHECK(v->IsFunction());
  v8::Local<v8::Function> func = v8::Local<v8::Function>::Cast(v);

  d->recv.Reset(isolate, func);
}

void Send(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  Deno* d = static_cast<Deno*>(isolate->GetData(0));
  DCHECK_EQ(d->isolate, isolate);

  v8::Locker locker(d->isolate);
  v8::EscapableHandleScope handle_scope(isolate);

  CHECK_EQ(args.Length(), 1);
  v8::Local<v8::Value> ab_v = args[0];
  CHECK(ab_v->IsArrayBufferView());
  auto buf = ExportBuf(isolate, v8::Local<v8::ArrayBufferView>::Cast(ab_v));

  DCHECK_EQ(d->currentArgs, nullptr);
  d->currentArgs = &args;

  d->cb(d, buf);

  // Buffer is only valid until the end of the callback.
  // TODO(piscisaureus):
  //   It's possible that data in the buffer is needed after the callback
  //   returns, e.g. when the handler offloads work to a thread pool, therefore
  //   make the callback responsible for releasing the buffer.
  FreeBuf(buf);

  d->currentArgs = nullptr;
}

bool Execute(v8::Local<v8::Context> context, const char* js_filename,
             const char* js_source) {
  auto* isolate = context->GetIsolate();
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);

  v8::Context::Scope context_scope(context);

  v8::TryCatch try_catch(isolate);

  auto name = v8_str(js_filename);
  auto source = v8_str(js_source);

  v8::ScriptOrigin origin(name);

  auto script = v8::Script::Compile(context, source, &origin);

  if (script.IsEmpty()) {
    DCHECK(try_catch.HasCaught());
    HandleException(context, try_catch.Exception());
    return false;
  }

  auto result = script.ToLocalChecked()->Run(context);

  if (result.IsEmpty()) {
    DCHECK(try_catch.HasCaught());
    HandleException(context, try_catch.Exception());
    return false;
  }

  return true;
}

void InitializeContext(v8::Isolate* isolate, v8::Local<v8::Context> context,
                       const char* js_filename, const std::string& js_source,
                       const std::string* source_map) {
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context);

  auto global = context->Global();

  auto deno_val = v8::Object::New(isolate);
  CHECK(global->Set(context, deno::v8_str("deno"), deno_val).FromJust());

  auto print_tmpl = v8::FunctionTemplate::New(isolate, Print);
  auto print_val = print_tmpl->GetFunction(context).ToLocalChecked();
  CHECK(deno_val->Set(context, deno::v8_str("print"), print_val).FromJust());

  auto recv_tmpl = v8::FunctionTemplate::New(isolate, Recv);
  auto recv_val = recv_tmpl->GetFunction(context).ToLocalChecked();
  CHECK(deno_val->Set(context, deno::v8_str("recv"), recv_val).FromJust());

  auto send_tmpl = v8::FunctionTemplate::New(isolate, Send);
  auto send_val = send_tmpl->GetFunction(context).ToLocalChecked();
  CHECK(deno_val->Set(context, deno::v8_str("send"), send_val).FromJust());

  // bool r = Execute(context, js_filename, js_source.c_str());
  v8::TryCatch try_catch(isolate);
  skip_onerror = true;
  {
    deno::Execute(context, js_filename, js_source.c_str());
    if (try_catch.HasCaught()) {
      exit(1);
    }

    if (source_map != nullptr) {
      CHECK_GT(source_map->length(), 1u);
      std::string set_source_map = "setMainSourceMap( " + *source_map + " )";
      CHECK_GT(set_source_map.length(), source_map->length());
      deno::Execute(context, "set_source_map.js", set_source_map.c_str());
      if (try_catch.HasCaught()) {
        exit(2);
      }
    }
  }
  skip_onerror = false;
}

void AddIsolate(Deno* d, v8::Isolate* isolate) {
  d->isolate = isolate;
  // Leaving this code here because it will probably be useful later on, but
  // disabling it now as I haven't got tests for the desired behavior.
  // d->isolate->SetCaptureStackTraceForUncaughtExceptions(true);
  // d->isolate->SetAbortOnUncaughtExceptionCallback(AbortOnUncaughtExceptionCallback);
  // d->isolate->AddMessageListener(MessageCallback2);
  // d->isolate->SetFatalErrorHandler(FatalErrorCallback2);
  d->isolate->SetPromiseRejectCallback(deno::ExitOnPromiseRejectCallback);
  d->isolate->SetData(0, d);
}

}  // namespace deno

extern "C" {

void deno_init() {
  // v8::V8::InitializeICUDefaultLocation(argv[0]);
  // v8::V8::InitializeExternalStartupData(argv[0]);
  auto* p = v8::platform::CreateDefaultPlatform();
  v8::V8::InitializePlatform(p);
  v8::V8::Initialize();
}

const char* deno_v8_version() { return v8::V8::GetVersion(); }

// TODO(ry) Remove these when we call deno_reply_start from Rust.
static char** global_argv;
static int global_argc;
char** deno_argv() { return global_argv; }
int deno_argc() { return global_argc; }

void deno_set_flags(int* argc, char** argv) {
  v8::V8::SetFlagsFromCommandLine(argc, argv, true);
  // TODO(ry) Remove these when we call deno_reply_start from Rust.
  global_argc = *argc;
  global_argv = reinterpret_cast<char**>(malloc(*argc * sizeof(char*)));
  for (int i = 0; i < *argc; i++) {
    global_argv[i] = strdup(argv[i]);
  }
}

const char* deno_last_exception(Deno* d) { return d->last_exception.c_str(); }

int deno_execute(Deno* d, const char* js_filename, const char* js_source) {
  auto* isolate = d->isolate;
  v8::Locker locker(isolate);
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);
  auto context = d->context.Get(d->isolate);
  return deno::Execute(context, js_filename, js_source) ? 1 : 0;
}

int deno_send(Deno* d, deno_buf buf) {
  v8::Locker locker(d->isolate);
  v8::Isolate::Scope isolate_scope(d->isolate);
  v8::HandleScope handle_scope(d->isolate);

  auto context = d->context.Get(d->isolate);
  v8::Context::Scope context_scope(context);

  v8::TryCatch try_catch(d->isolate);

  auto recv = d->recv.Get(d->isolate);
  if (recv.IsEmpty()) {
    d->last_exception = "deno.recv has not been called.";
    return 0;
  }

  v8::Local<v8::Value> args[1];
  args[0] = deno::ImportBuf(d->isolate, buf);
  recv->Call(context->Global(), 1, args);

  if (try_catch.HasCaught()) {
    deno::HandleException(context, try_catch.Exception());
    return 0;
  }

  return 1;
}

void deno_set_response(Deno* d, deno_buf buf) {
  auto ab = deno::ImportBuf(d->isolate, buf);
  d->currentArgs->GetReturnValue().Set(ab);
}

void deno_delete(Deno* d) {
  d->isolate->Dispose();
  delete d;
}

void deno_terminate_execution(Deno* d) { d->isolate->TerminateExecution(); }

}  // extern "C"
