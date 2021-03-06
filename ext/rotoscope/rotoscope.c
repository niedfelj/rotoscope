#include <errno.h>
#include <ruby.h>
#include <ruby/debug.h>
#include <ruby/intern.h>
#include <ruby/version.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/file.h>

#include "callsite.h"
#include "rotoscope.h"
#include "stack.h"
#include "strmemo.h"
#include "tracepoint.h"

VALUE cRotoscope, cTracePoint;

// recursive with singleton2str
static rs_class_desc_t class2str(VALUE klass);

static unsigned long gettid() {
  return NUM2ULONG(rb_obj_id(rb_thread_current()));
}

static int write_csv_header(FILE *log, const char *header) {
  return fprintf(log, "%s\n", header);
}

static const char *evflag2name(rb_event_flag_t evflag) {
  switch (evflag) {
    case RUBY_EVENT_CALL:
    case RUBY_EVENT_C_CALL:
      return "call";
    case RUBY_EVENT_RETURN:
    case RUBY_EVENT_C_RETURN:
      return "return";
    default:
      return "unknown";
  }
}

static bool rejected_path(VALUE path, Rotoscope *config) {
  for (unsigned long i = 0; i < config->blacklist_size; i++) {
    if (strstr(StringValueCStr(path), config->blacklist[i])) return true;
  }

  return false;
}

static VALUE class_of_singleton(VALUE klass) {
  return rb_iv_get(klass, "__attached__");
}

static bool is_class_singleton(VALUE klass) {
  VALUE obj = class_of_singleton(klass);
  return (RB_TYPE_P(obj, T_MODULE) || RB_TYPE_P(obj, T_CLASS));
}

static VALUE singleton2str(VALUE klass) {
  if (is_class_singleton(klass)) {
    VALUE obj = class_of_singleton(klass);
    VALUE cached_lookup = rb_class_path_cached(obj);
    VALUE name = (NIL_P(cached_lookup)) ? rb_class_name(obj) : cached_lookup;
    return name;
  } else  // singleton of an instance
  {
    return singleton2str(CLASS_OF(klass));
  }
}

static rs_class_desc_t class2str(VALUE klass) {
  rs_class_desc_t real_class;
  real_class.method_level = INSTANCE_METHOD;

  VALUE cached_lookup = rb_class_path_cached(klass);
  if (RTEST(cached_lookup)) {
    real_class.name = cached_lookup;
  } else {
    if (FL_TEST(klass, FL_SINGLETON)) {
      real_class.name = singleton2str(klass);
      if (is_class_singleton(klass)) {
        real_class.method_level = CLASS_METHOD;
      }
    } else {
      real_class.name = rb_class_path(klass);
    }
  }

  return real_class;
}

static rs_callsite_t tracearg_path(rb_trace_arg_t *trace_arg) {
  switch (rb_tracearg_event_flag(trace_arg)) {
    case RUBY_EVENT_C_RETURN:
    case RUBY_EVENT_C_CALL:
      return c_callsite(trace_arg);
    default:
      return ruby_callsite(trace_arg);
  }
}

static rs_class_desc_t tracearg_class(rb_trace_arg_t *trace_arg) {
  VALUE klass;
  VALUE self = rb_tracearg_self(trace_arg);

  if (RB_TYPE_P(self, T_MODULE) || RB_TYPE_P(self, T_OBJECT)) {
    klass = CLASS_OF(self);
  } else if (RB_TYPE_P(self, T_CLASS)) {
    // Does the object have an attached singleton?
    // If not, name based on self instead of its singleton
    klass = (FL_TEST(CLASS_OF(self), FL_SINGLETON)) ? CLASS_OF(self) : self;
  } else {
    klass = rb_tracearg_defined_class(trace_arg);
  }

  return class2str(klass);
}

static VALUE tracearg_method_name(rb_trace_arg_t *trace_arg) {
  return rb_sym2str(rb_tracearg_method_id(trace_arg));
}

static rs_tracepoint_t extract_full_tracevals(rb_trace_arg_t *trace_arg,
                                              const rs_callsite_t *callsite) {
  rs_class_desc_t method_owner = tracearg_class(trace_arg);
  rb_event_flag_t event_flag = rb_tracearg_event_flag(trace_arg);

  VALUE method_name = tracearg_method_name(trace_arg);
  VALUE filepath = callsite->filepath;

  return (rs_tracepoint_t){.event = evflag2name(event_flag),
                           .entity = method_owner.name,
                           .filepath = filepath,
                           .method_name = method_name,
                           .method_level = method_owner.method_level,
                           .lineno = callsite->lineno};
}

static bool in_fork(Rotoscope *config) { return config->pid != getpid(); }

static bool tracecmp(rs_tracepoint_t *a, rs_tracepoint_t *b) {
  return (!rb_str_cmp(a->method_name, b->method_name) &&
          !rb_str_cmp(a->entity, b->entity) &&
          a->method_level == b->method_level);
}

static void log_raw_trace(FILE *stream, rs_tracepoint_t trace) {
  fprintf(stream, RS_CSV_FORMAT "\n", RS_CSV_VALUES(trace));
}

unsigned char output_buffer[500];
static void log_stack_frame(FILE *stream, rs_stack_t *stack,
                            rs_strmemo_t **call_memo, rs_tracepoint_t trace,
                            rb_event_flag_t event) {
  if (event & EVENT_CALL) {
    rs_stack_frame_t frame = rs_stack_push(stack, trace);
    sprintf((char *)output_buffer, RS_FLATTENED_CSV_FORMAT "\n",
            RS_FLATTENED_CSV_VALUES(frame));

    if (rs_strmemo_uniq(call_memo, output_buffer)) {
      fputs((char *)output_buffer, stream);
    }
  } else if (event & EVENT_RETURN) {
    if (tracecmp(&trace, &rs_stack_peek(stack)->tp)) rs_stack_pop(stack);
  }
}

static void event_hook(VALUE tpval, void *data) {
  Rotoscope *config = (Rotoscope *)data;

  if (config->tid != gettid()) return;
  if (in_fork(config)) {
    rb_tracepoint_disable(config->tracepoint);
    config->state = RS_OPEN;
    return;
  }

  rb_trace_arg_t *trace_arg = rb_tracearg_from_tracepoint(tpval);
  rs_callsite_t trace_path = tracearg_path(trace_arg);

  if (rejected_path(trace_path.filepath, config)) return;

  rs_tracepoint_t trace = extract_full_tracevals(trace_arg, &trace_path);
  if (!strcmp("Rotoscope", StringValueCStr(trace.entity))) return;

  if (config->flatten_output) {
    rb_event_flag_t event_flag = rb_tracearg_event_flag(trace_arg);
    log_stack_frame(config->log, &config->stack, &config->call_memo, trace,
                    event_flag);
  } else {
    log_raw_trace(config->log, trace);
  }
}

static void close_log_handle(Rotoscope *config) {
  if (config->log) {
    // Stop tracing so the event hook isn't called with a NULL log
    if (config->state == RS_TRACING) {
      // During process cleanup, event hooks are removed and tracepoint may have
      // already have been GCed, so we need a sanity check before disabling the
      // tracepoint.
      if (RB_TYPE_P(config->tracepoint, T_DATA) &&
          CLASS_OF(config->tracepoint) == cTracePoint) {
        rb_tracepoint_disable(config->tracepoint);
      }
    }

    if (in_fork(config)) {
      close(fileno(config->log));
    } else {
      fclose(config->log);
    }
    config->log = NULL;
  }
}

static void rs_gc_mark(Rotoscope *config) {
  rb_gc_mark(config->log_path);
  rb_gc_mark(config->tracepoint);
  rs_stack_mark(&config->stack);
}

void rs_dealloc(Rotoscope *config) {
  close_log_handle(config);
  rs_stack_free(&config->stack);
  rs_strmemo_free(config->call_memo);
  xfree(config->blacklist);
  xfree(config);
}

static VALUE rs_alloc(VALUE klass) {
  Rotoscope *config;
  VALUE self =
      Data_Make_Struct(klass, Rotoscope, rs_gc_mark, rs_dealloc, config);
  config->log_path = Qnil;
  config->tracepoint = rb_tracepoint_new(Qnil, EVENT_CALL | EVENT_RETURN,
                                         event_hook, (void *)config);
  config->pid = getpid();
  config->tid = gettid();
  return self;
}

static Rotoscope *get_config(VALUE self) {
  Rotoscope *config;
  Data_Get_Struct(self, Rotoscope, config);
  return config;
}

void copy_blacklist(Rotoscope *config, VALUE blacklist) {
  Check_Type(blacklist, T_ARRAY);

  size_t blacklist_malloc_size =
      RARRAY_LEN(blacklist) * sizeof(*config->blacklist);

  for (long i = 0; i < RARRAY_LEN(blacklist); i++) {
    VALUE ruby_string = RARRAY_AREF(blacklist, i);
    Check_Type(ruby_string, T_STRING);
    blacklist_malloc_size += RSTRING_LEN(ruby_string) + 1;
  }

  config->blacklist = ruby_xmalloc(blacklist_malloc_size);
  config->blacklist_size = RARRAY_LEN(blacklist);
  char *str = (char *)(config->blacklist + config->blacklist_size);

  for (unsigned long i = 0; i < config->blacklist_size; i++) {
    VALUE ruby_string = RARRAY_AREF(blacklist, i);

    config->blacklist[i] = str;
    memcpy(str, RSTRING_PTR(ruby_string), RSTRING_LEN(ruby_string));
    str += RSTRING_LEN(ruby_string);
    *str = '\0';
    str++;
  }
}

VALUE initialize(int argc, VALUE *argv, VALUE self) {
  Rotoscope *config = get_config(self);
  VALUE output_path, blacklist, flatten;

  rb_scan_args(argc, argv, "12", &output_path, &blacklist, &flatten);
  Check_Type(output_path, T_STRING);

  if (!NIL_P(blacklist)) {
    copy_blacklist(config, blacklist);
  }

  config->flatten_output = RTEST(flatten);
  config->log_path = output_path;
  config->log = fopen(StringValueCStr(config->log_path), "w");

  if (config->log == NULL) {
    fprintf(stderr, "\nERROR: Failed to open file handle at %s (%s)\n",
            StringValueCStr(config->log_path), strerror(errno));
    exit(1);
  }

  if (config->flatten_output)
    write_csv_header(config->log, RS_FLATTENED_CSV_HEADER);
  else
    write_csv_header(config->log, RS_CSV_HEADER);

  rs_stack_init(&config->stack, STACK_CAPACITY);
  config->call_memo = NULL;
  config->state = RS_OPEN;
  return self;
}

VALUE rotoscope_start_trace(VALUE self) {
  Rotoscope *config = get_config(self);
  rb_tracepoint_enable(config->tracepoint);
  config->state = RS_TRACING;
  return Qnil;
}

VALUE rotoscope_stop_trace(VALUE self) {
  Rotoscope *config = get_config(self);
  if (rb_tracepoint_enabled_p(config->tracepoint)) {
    rb_tracepoint_disable(config->tracepoint);
    config->state = RS_OPEN;
  }

  return Qnil;
}

VALUE rotoscope_log_path(VALUE self) {
  Rotoscope *config = get_config(self);
  return config->log_path;
}

VALUE rotoscope_mark(VALUE self) {
  Rotoscope *config = get_config(self);
  if (config->log != NULL && !in_fork(config)) {
    rs_stack_reset(&config->stack, STACK_CAPACITY);
    rs_strmemo_free(config->call_memo);
    fprintf(config->log, "---\n");
  }
  return Qnil;
}

VALUE rotoscope_close(VALUE self) {
  Rotoscope *config = get_config(self);
  if (config->state == RS_CLOSED) {
    return Qtrue;
  }
  close_log_handle(config);
  config->state = RS_CLOSED;
  return Qtrue;
}

VALUE rotoscope_trace(VALUE self) {
  rotoscope_start_trace(self);
  return rb_ensure(rb_yield, Qundef, rotoscope_stop_trace, self);
}

VALUE rotoscope_state(VALUE self) {
  Rotoscope *config = get_config(self);
  switch (config->state) {
    case RS_OPEN:
      return ID2SYM(rb_intern("open"));
    case RS_TRACING:
      return ID2SYM(rb_intern("tracing"));
    default:
      return ID2SYM(rb_intern("closed"));
  }
}

void Init_rotoscope(void) {
  cTracePoint = rb_const_get(rb_cObject, rb_intern("TracePoint"));

  cRotoscope = rb_define_class("Rotoscope", rb_cObject);
  rb_define_alloc_func(cRotoscope, rs_alloc);
  rb_define_method(cRotoscope, "initialize", initialize, -1);
  rb_define_method(cRotoscope, "trace", (VALUE(*)(ANYARGS))rotoscope_trace, 0);
  rb_define_method(cRotoscope, "mark", (VALUE(*)(ANYARGS))rotoscope_mark, 0);
  rb_define_method(cRotoscope, "close", (VALUE(*)(ANYARGS))rotoscope_close, 0);
  rb_define_method(cRotoscope, "start_trace",
                   (VALUE(*)(ANYARGS))rotoscope_start_trace, 0);
  rb_define_method(cRotoscope, "stop_trace",
                   (VALUE(*)(ANYARGS))rotoscope_stop_trace, 0);
  rb_define_method(cRotoscope, "log_path",
                   (VALUE(*)(ANYARGS))rotoscope_log_path, 0);
  rb_define_method(cRotoscope, "state", (VALUE(*)(ANYARGS))rotoscope_state, 0);

  init_callsite();
}
