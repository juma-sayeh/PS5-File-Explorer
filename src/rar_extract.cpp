/*
 * BFpilot - UnRAR streaming adapter.
 */

#include "rar_extract.h"

#include <exception>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "unrar/rar.hpp"

static thread_local const rar_extract_opts_t *g_rar_opts = nullptr;

#ifdef BFPILOT_UNRAR_STREAM
extern "C" void bfpilot_unrar_ui_reset(void);
extern "C" const char *bfpilot_unrar_ui_last_message(void);
#endif

extern "C" bool
bfpilot_unrar_stream_active(void) {
  return g_rar_opts != nullptr;
}

extern "C" bool
bfpilot_unrar_cancelled(void) {
  return g_rar_opts && g_rar_opts->cancel_cb &&
    g_rar_opts->cancel_cb(g_rar_opts->opaque);
}

extern "C" int
bfpilot_unrar_stream_read(void *data, size_t size) {
  if(!g_rar_opts || !g_rar_opts->read_cb) return -1;
  if(g_rar_opts->cancel_cb && g_rar_opts->cancel_cb(g_rar_opts->opaque)) {
    return -1;
  }

  size_t read_size = 0;
  if(g_rar_opts->read_cb(g_rar_opts->opaque, data, size, &read_size) != 0) {
    return -1;
  }
  if(read_size > (size_t)INT_MAX) read_size = (size_t)INT_MAX;
  return (int)read_size;
}

static std::wstring
utf8_to_wide(const char *src) {
  if(!src || !*src) return std::wstring();
  size_t len = strlen(src);
  std::vector<wchar> out(len + 1);
  UtfToWide(src, out.data(), out.size());
  return std::wstring(out.data());
}

static const char *
rar_error_text(RAR_EXIT code) {
  switch(code) {
  case RARX_SUCCESS:   return "ok";
  case RARX_WARNING:   return "rar extraction warning";
  case RARX_FATAL:     return "rar extraction failed";
  case RARX_CRC:       return "rar checksum failed";
  case RARX_LOCK:      return "rar archive is locked";
  case RARX_WRITE:     return "rar write failed";
  case RARX_OPEN:      return "rar open failed";
  case RARX_USERERROR: return "rar options error";
  case RARX_MEMORY:    return "rar ran out of memory";
  case RARX_CREATE:    return "rar could not create output";
  case RARX_NOFILES:   return "rar contained no files to extract";
  case RARX_BADPWD:    return "bad rar password";
  case RARX_READ:      return "rar read failed";
  case RARX_BADARC:    return "bad rar archive";
  case RARX_USERBREAK: return "rar extraction cancelled";
  default:             return "rar extraction failed";
  }
}

static void
set_err(char *err, size_t err_size, const char *msg) {
  if(err_size == 0) return;
  snprintf(err, err_size, "%s", msg ? msg : "rar extraction failed");
}

static void
set_rar_err(char *err, size_t err_size, RAR_EXIT code) {
  if(err_size == 0) return;

  const char *base = rar_error_text(code);
#ifdef BFPILOT_UNRAR_STREAM
  const char *detail = bfpilot_unrar_ui_last_message();
  if(detail && detail[0]) {
    snprintf(err, err_size, "%s: %s", base, detail);
    return;
  }
#endif
  set_err(err, err_size, base);
}

static int
rar_extract_run(const rar_extract_opts_t *opts, bool stream,
                char *err, size_t err_size) {
  if(!opts || !opts->archive_name || !opts->dest_dir ||
     (stream && !opts->read_cb)) {
    set_err(err, err_size, "bad rar extract request");
    return -1;
  }

  g_rar_opts = opts;
  try {
#ifdef BFPILOT_UNRAR_STREAM
    bfpilot_unrar_ui_reset();
#endif
    ErrHandler.Clean();
    ErrHandler.SetSilent(true);

    CommandData cmd;
    cmd.Command = L"X";
    if(stream) cmd.UseStdin = L"stdin";
    cmd.AddArcName(utf8_to_wide(opts->archive_name));
    cmd.FileArgs.AddString(MASKALL);
    cmd.ExtrPath = utf8_to_wide(opts->dest_dir);
    AddEndSlash(cmd.ExtrPath);
    cmd.Overwrite = OVERWRITE_ALL;
    cmd.AllYes = true;
    cmd.DisableCopyright = true;
    cmd.DisableDone = true;
    cmd.DisableNames = true;
    cmd.DisablePercentage = true;
    cmd.DisableComment = true;
    cmd.SkipSymLinks = true;
    cmd.AbsoluteLinks = false;
    cmd.ProcessOwners = false;
    cmd.OpenShared = true;
    cmd.Threads = 1;
    cmd.WinSizeLimit = opts->dictionary_limit ? opts->dictionary_limit :
      (64ULL * 1024ULL * 1024ULL);

    if(opts->password && opts->password[0]) {
      cmd.Password.Set(utf8_to_wide(opts->password).c_str());
    }

    CmdExtract extract(&cmd);
    extract.DoExtract();

    RAR_EXIT code = ErrHandler.GetErrorCode();
    g_rar_opts = nullptr;
    if(code != RARX_SUCCESS) {
      set_rar_err(err, err_size, code);
      return -1;
    }
    return 0;
  } catch(RAR_EXIT code) {
    g_rar_opts = nullptr;
    set_rar_err(err, err_size, code);
    return -1;
  } catch(std::bad_alloc&) {
    g_rar_opts = nullptr;
    set_err(err, err_size, "rar ran out of memory");
    return -1;
  } catch(const std::exception& ex) {
    g_rar_opts = nullptr;
    snprintf(err, err_size, "rar exception: %s",
             ex.what() ? ex.what() : "unknown");
    return -1;
  } catch(...) {
    g_rar_opts = nullptr;
    set_err(err, err_size, "rar unknown exception");
    return -1;
  }
}

int
rar_extract_stream(const rar_extract_opts_t *opts, char *err, size_t err_size) {
  return rar_extract_run(opts, true, err, err_size);
}

int
rar_extract_file(const rar_extract_opts_t *opts, char *err, size_t err_size) {
  return rar_extract_run(opts, false, err, err_size);
}
