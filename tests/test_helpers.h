/**
 * @file
 *
 * Shared helpers for the Ghoti.io Font unit tests.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GFNT_TEST_HELPERS_H
#define GHOTI_IO_GFNT_TEST_HELPERS_H

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/path.h>
#include <ghoti.io/font/font.h>

#include "failing_allocator.h"

namespace gfnttest {

/**
 * Directory holding the checked-in fixtures. The Makefile bakes in
 * GFNT_TEST_DATA so the binaries can run from the build tree; the environment
 * variable wins when it is set, and there is a relative fallback for a manual
 * build.
 */
inline std::string data_dir() {
  const char * env = std::getenv("GFNT_TEST_DATA");
  if (env) {
    return std::string(env);
  }
#ifdef GFNT_TEST_DATA
  return std::string(GFNT_TEST_DATA);
#else
  return std::string("tests/data");
#endif
}

/** A path that is guaranteed not to exist. */
inline const char * missing_path() {
  return "/nonexistent/ghoti.io/definitely/not/here.dat";
}

/** Path to a checked-in fixture. */
inline std::string data(const std::string & relative) {
  char joined[1024];
  if (gcu_path_join(GCU_PATH_NATIVE, data_dir().c_str(), relative.c_str(),
          joined, sizeof(joined), nullptr)
      != GCU_PATH_OK) {
    // Only reachable if the data directory is absurdly long. Hand back
    // something that cannot open, rather than a truncated path that could.
    return std::string(missing_path());
  }
  return std::string(joined);
}

/**
 * A file written to a temporary path and removed when the object goes out of
 * scope. Used only by the tests that exercise the file entry points; the rest
 * parse from memory, which is the point of the stream API.
 *
 * The file comes from cutil, which creates it in whatever directory
 * gcu_path_temp_dir() names - so $TMPDIR is honoured rather than /tmp being
 * assumed - and which chooses the name and creates the file in one step that
 * fails if the name is taken. The helper this replaced invented a name,
 * deleted it, and reopened it with an extension appended, leaving a window in
 * which something else could take the path. The extension itself is gone with
 * it: nothing in this library decides anything from a file's name.
 *
 * The handle stays open for the object's lifetime. A test that reopens
 * path() to write to it is reopening the same file, which is what the dump
 * round-trip tests do.
 */
class TempFile {
public:
  explicit TempFile(const std::string & contents) {
    if (gcu_file_temp_create(&temp_, nullptr, "gfnt_test", nullptr)
        != GCU_FILE_OK) {
      return;
    }
    path_ = std::string(gcu_file_temp_path(&temp_));
    FILE * stream = gcu_file_temp_stream(&temp_);
    if (!contents.empty()
        && fwrite(contents.data(), 1, contents.size(), stream)
            != contents.size()) {
      gcu_file_temp_abort(&temp_);
      return;
    }
    if (fflush(stream) != 0) {
      gcu_file_temp_abort(&temp_);
      return;
    }
    valid_ = true;
  }

  TempFile(const TempFile &) = delete;
  TempFile & operator=(const TempFile &) = delete;

  /// Closes the file and deletes it. Accepts an already-spent handle, so it
  /// is correct whether or not the constructor got that far.
  ~TempFile() { gcu_file_temp_abort(&temp_); }

  const char * path() const { return path_.c_str(); }
  bool valid() const { return valid_; }

private:
  GCU_File_Temp temp_{};
  std::string path_;
  bool valid_ = false;
};

/**
 * A `FILE *` that accepts a set number of writes and fails every one after.
 *
 * The dumpers are almost all error handling by line count - every `fprintf`
 * is checked - and until this existed not one of those arms had ever run.
 * A test can only see them by handing over a stream that fails, and it has
 * to fail on demand rather than always, because failing on the first write
 * exercises exactly one of them.
 *
 * Buffering is off, so one `fprintf` is one write: constructed with `n`, the
 * sink serves n writes and fails the one after. Sweeping n from zero upwards
 * walks the failure through the whole of a dump.
 *
 * `fopencookie` is glibc's, and Windows has nothing like it: a CRT `FILE *`
 * cannot be given callbacks. There the failure is served one step higher, at
 * the library's own call. The test links with `-Wl,--wrap=__mingw_fprintf`
 * (the name MinGW's headers give `fprintf` in C), so every `fprintf` the
 * dumpers make lands in the wrapper at the end of this header, which counts
 * the ones aimed at the sink and returns -1 once the budget is spent. The
 * dumpers see exactly what they see on Linux - `fprintf` answering negative
 * with errno set - so the same arms run. What is not exercised on Windows is
 * the CRT turning a failed write into that answer, which is the CRT's code.
 * If the library ever writes through anything but `fprintf`, the wrapper
 * stops seeing it, the sweeps find no failures to count, and their floors
 * fail - so this cannot go quietly vacuous.
 */
#ifdef _WIN32
class FailingSink {
public:
  explicit FailingSink(size_t allow) : remaining_(allow) {
    // The bytes that are allowed through have to go somewhere real, so that
    // a write that is not refused behaves as a write.
    file_ = fopen("NUL", "wb");
    if (file_) {
      setvbuf(file_, nullptr, _IONBF, 0);
      EXPECT_EQ(active_, nullptr) << "one FailingSink at a time";
      active_ = this;
    }
  }

  FailingSink(const FailingSink &) = delete;
  FailingSink & operator=(const FailingSink &) = delete;

  ~FailingSink() {
    if (file_) {
      active_ = nullptr;
      fclose(file_);
    }
  }

  FILE * get() const { return file_; }

  /** Whether the write budget ran out, i.e. a failure was actually served. */
  bool failed() const { return failed_; }

  /**
   * Called by the fprintf wrapper for every call the linked code makes.
   * Returns false when the call must fail.
   */
  static bool admit(FILE * stream) {
    FailingSink * self = active_;
    if (!self || stream != self->file_) {
      return true;
    }
    if (self->remaining_ == 0) {
      self->failed_ = true;
      errno = ENOSPC;
      return false;
    }
    self->remaining_--;
    return true;
  }

private:
  static inline FailingSink * active_ = nullptr;
  size_t remaining_;
  bool failed_ = false;
  FILE * file_ = nullptr;
};
#else
class FailingSink {
public:
  explicit FailingSink(size_t allow) : remaining_(allow) {
    cookie_io_functions_t fns = {};
    fns.write = &FailingSink::write_cb;
    file_ = fopencookie(this, "w", fns);
    if (file_) {
      setvbuf(file_, nullptr, _IONBF, 0);
    }
  }

  FailingSink(const FailingSink &) = delete;
  FailingSink & operator=(const FailingSink &) = delete;

  ~FailingSink() {
    if (file_) {
      fclose(file_);
    }
  }

  FILE * get() const { return file_; }

  /** Whether the write budget ran out, i.e. a failure was actually served. */
  bool failed() const { return failed_; }

private:
  static ssize_t write_cb(void * cookie, const char * buffer, size_t size) {
    (void)buffer;
    FailingSink * self = static_cast<FailingSink *>(cookie);
    if (self->remaining_ == 0) {
      self->failed_ = true;
      errno = ENOSPC;
      return -1;
    }
    self->remaining_--;
    return static_cast<ssize_t>(size);
  }

  size_t remaining_;
  bool failed_ = false;
  FILE * file_ = nullptr;
};
#endif

/**
 * A stream that collects what is written to it, for a test that inspects a
 * dump's text.
 *
 * `open_memstream` is POSIX 2008 and the Windows CRT has no counterpart, so
 * there the bytes go to a temporary file (binary, so nothing rewrites line
 * endings) and are read back. finish() closes the stream either way; get()
 * must not be used after it.
 */
class CapturedOutput {
public:
  CapturedOutput() {
#ifdef _WIN32
    if (gcu_file_temp_create(&temp_, nullptr, "gfnt_capture", nullptr)
        == GCU_FILE_OK) {
      file_ = gcu_file_temp_stream(&temp_);
    }
#else
    file_ = open_memstream(&buffer_, &size_);
#endif
  }

  CapturedOutput(const CapturedOutput &) = delete;
  CapturedOutput & operator=(const CapturedOutput &) = delete;

  ~CapturedOutput() {
#ifdef _WIN32
    gcu_file_temp_abort(&temp_);
#else
    if (file_) {
      fclose(file_);
    }
    free(buffer_);
#endif
  }

  FILE * get() const { return file_; }

  /** Everything written so far. Ends the capture. */
  std::string finish() {
    std::string text;
    if (!file_) {
      return text;
    }
#ifdef _WIN32
    fflush(file_);
    rewind(file_);
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, file_)) > 0) {
      text.append(chunk, got);
    }
    // The handle belongs to temp_, which the destructor closes.
#else
    fclose(file_);
    text.assign(buffer_, size_);
#endif
    file_ = nullptr;
    return text;
  }

private:
  FILE * file_ = nullptr;
#ifdef _WIN32
  GCU_File_Temp temp_{};
#else
  char * buffer_ = nullptr;
  size_t size_ = 0;
#endif
};

} // namespace gfnttest

#ifdef _WIN32
/**
 * The other half of the Windows FailingSink: the Makefile links every test
 * with `-Wl,--wrap=__mingw_fprintf`, which sends each call to it here. Calls
 * aimed at anything but the active sink pass straight through.
 *
 * Inline and `used`, so that every test program has exactly one definition
 * whether or not it uses FailingSink - the wrap applies to all of them, and a
 * program without the wrapper would not link.
 */
extern "C" __attribute__((used)) inline int __wrap___mingw_fprintf(
    FILE * stream, const char * format, ...) {
  if (!gfnttest::FailingSink::admit(stream)) {
    return -1;
  }
  va_list args;
  va_start(args, format);
  int result = __mingw_vfprintf(stream, format, args);
  va_end(args);
  return result;
}
#endif

#endif // GHOTI_IO_GFNT_TEST_HELPERS_H
