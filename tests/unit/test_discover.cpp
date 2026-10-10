/**
 * @file
 *
 * GFNT_FontSet: one record per face, and the names that point at one.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "test_helpers.h"

#include <ghoti.io/cutil/dir.h>
#include <ghoti.io/cutil/env.h>
#include <ghoti.io/font/discover.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/wait.h>
#endif

namespace {

class TempTree {
public:
  TempTree() {
    char * created = nullptr;
    if (gcu_dir_temp_create(nullptr, "gfnt-discover", nullptr, &created)
        != GCU_FILE_OK) {
      return;
    }
    path_ = created;
    gcu_dir_free_path(nullptr, created);
  }

  ~TempTree() {
    if (!path_.empty()) {
      remove_tree(path_);
    }
  }

  bool ok() const { return !path_.empty(); }
  const std::string & path() const { return path_; }

  std::string child(const std::string & relative) const {
    char joined[4096];
    if (gcu_path_join(GCU_PATH_NATIVE, path_.c_str(), relative.c_str(), joined,
            sizeof joined, nullptr)
        != GCU_PATH_OK) {
      return std::string();
    }
    return std::string(joined);
  }

  bool mkdir_p(const std::string & relative) const {
    std::string full = relative.empty() ? path_ : child(relative);
    return gcu_dir_create_all(full.c_str(), nullptr) == GCU_FILE_OK;
  }

private:
  static void remove_tree(const std::string & path) {
    GCU_Dir dir;
    if (gcu_dir_open(&dir, path.c_str(), nullptr) != GCU_FILE_OK) {
      gcu_dir_close(&dir);
      unlink(path.c_str());
      return;
    }
    for (;;) {
      const char * name = nullptr;
      GCU_File_Type type = GCU_FILE_TYPE_REGULAR;
      bool done = false;
      if (gcu_dir_read(&dir, &name, &type, &done) != GCU_FILE_OK || done) {
        break;
      }
      char joined[4096];
      if (gcu_path_join(GCU_PATH_NATIVE, path.c_str(), name, joined,
              sizeof joined, nullptr)
          != GCU_PATH_OK) {
        continue;
      }
      if (type == GCU_FILE_TYPE_DIRECTORY) {
        remove_tree(joined);
      } else {
        unlink(joined);
      }
    }
    gcu_dir_close(&dir);
    rmdir(path.c_str());
  }

  std::string path_;
};

class Set {
public:
  GFNT_FontSet * set = nullptr;
  ~Set() { gfnt_fontset_free(set); }
};

bool copy_file(const std::string & from, const std::string & to) {
  FILE * in = fopen(from.c_str(), "rb");
  FILE * out = fopen(to.c_str(), "wb");
  char buffer[8192];
  bool ok = in && out;
  if (ok) {
    size_t got = 0;
    while ((got = fread(buffer, 1, sizeof buffer, in)) > 0) {
      if (fwrite(buffer, 1, got, out) != got) {
        ok = false;
        break;
      }
    }
    if (ferror(in)) {
      ok = false;
    }
  }
  if (in) {
    fclose(in);
  }
  if (out) {
    fclose(out);
  }
  return ok;
}

bool write_bytes(const std::string & path, const void * data, size_t size) {
  FILE * out = fopen(path.c_str(), "wb");
  if (!out) {
    return false;
  }
  bool ok = fwrite(data, 1, size, out) == size;
  fclose(out);
  return ok;
}

bool write_text(const std::string & path, const std::string & text) {
  return write_bytes(path, text.data(), text.size());
}

bool patch_fs_selection(const std::string & path, unsigned bits) {
  FILE * file = fopen(path.c_str(), "r+b");
  unsigned char header[12];
  unsigned tables;
  unsigned i;
  bool found = false;
  if (!file || fread(header, 1, sizeof header, file) != sizeof header) {
    if (file) {
      fclose(file);
    }
    return false;
  }
  tables = ((unsigned)header[4] << 8) | header[5];
  for (i = 0; i < tables; i++) {
    unsigned char rec[16];
    if (fread(rec, 1, sizeof rec, file) != sizeof rec) {
      break;
    }
    if (rec[0] != 'O' || rec[1] != 'S' || rec[2] != '/' || rec[3] != '2') {
      continue;
    }
    unsigned offset = ((unsigned)rec[8] << 24) | ((unsigned)rec[9] << 16)
        | ((unsigned)rec[10] << 8) | rec[11];
    unsigned char value[2] = {
      (unsigned char)((bits >> 8) & 0xff),
      (unsigned char)(bits & 0xff),
    };
    if (fseek(file, (long)offset + 62, SEEK_SET) == 0) {
      found = fwrite(value, 1, 2, file) == 2;
    }
    break;
  }
  fclose(file);
  return found;
}

const GFNT_FontRecord * find_ending(const GFNT_FontSet * set,
    const char * suffix) {
  size_t suffix_len = std::strlen(suffix);
  for (size_t i = 0; i < gfnt_fontset_count(set); i++) {
    const GFNT_FontRecord * rec = gfnt_fontset_at(set, i);
    size_t path_len = std::strlen(rec->path);
    if (path_len >= suffix_len
        && std::strcmp(rec->path + path_len - suffix_len, suffix) == 0) {
      return rec;
    }
  }
  return nullptr;
}

const GFNT_FontSkip * find_skip_ending(const GFNT_FontSet * set,
    const char * suffix) {
  size_t suffix_len = std::strlen(suffix);
  for (size_t i = 0; i < gfnt_fontset_skip_count(set); i++) {
    const GFNT_FontSkip * skip = gfnt_fontset_skip_at(set, i);
    size_t path_len = std::strlen(skip->path);
    if (path_len >= suffix_len
        && std::strcmp(skip->path + path_len - suffix_len, suffix) == 0) {
      return skip;
    }
  }
  return nullptr;
}

size_t count_containing(const GFNT_FontSet * set, const char * fragment) {
  size_t found = 0;
  for (size_t i = 0; i < gfnt_fontset_count(set); i++) {
    if (std::strstr(gfnt_fontset_at(set, i)->path, fragment)) {
      found++;
    }
  }
  return found;
}

GFNT_Result scan_one(const std::string & dir, const GFNT_FontAlias * aliases,
    size_t alias_count, GFNT_FontSet ** out, GFNT_Error * error) {
  const char * dirs[] = { dir.c_str() };
  return gfnt_fontset_scan(dirs, 1, aliases, alias_count, nullptr, out, error);
}

#if defined(__linux__)

struct IsolateReport {
  int ok;
  char message[512];
};

void report_fail(IsolateReport * report, const char * message) {
  report->ok = 0;
  std::snprintf(report->message, sizeof report->message, "%s", message);
}

bool enter_namespace(IsolateReport * report) {
  uid_t uid = getuid();
  gid_t gid = getgid();
  if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
    report_fail(report, "unshare failed");
    return false;
  }
  char map[64];
  int fd = open("/proc/self/uid_map", O_WRONLY);
  if (fd < 0) {
    report_fail(report, "uid_map failed");
    return false;
  }
  std::snprintf(map, sizeof map, "0 %u 1\n", uid);
  if (write(fd, map, std::strlen(map)) < 0) {
    close(fd);
    report_fail(report, "writing uid_map failed");
    return false;
  }
  close(fd);
  fd = open("/proc/self/setgroups", O_WRONLY);
  if (fd >= 0) {
    if (write(fd, "deny", 4) < 0) {
      close(fd);
      report_fail(report, "setgroups failed");
      return false;
    }
    close(fd);
  }
  fd = open("/proc/self/gid_map", O_WRONLY);
  if (fd < 0) {
    report_fail(report, "gid_map failed");
    return false;
  }
  std::snprintf(map, sizeof map, "0 %u 1\n", gid);
  if (write(fd, map, std::strlen(map)) < 0) {
    close(fd);
    report_fail(report, "writing gid_map failed");
    return false;
  }
  close(fd);
  if (mount("none", "/usr/share/fonts", "tmpfs", 0, "") != 0) {
    report_fail(report, "mount of /usr/share/fonts failed");
    return false;
  }
  if (access("/usr/local/share/fonts", F_OK) == 0
      && mount("none", "/usr/local/share/fonts", "tmpfs", 0, "") != 0) {
    report_fail(report, "mount of /usr/local/share/fonts failed");
    return false;
  }
  return true;
}

struct EnvSlot {
  const char * name;
  bool had;
  char old[4096];
};

void env_set(EnvSlot * slot, const char * name, const char * value) {
  const char * current = std::getenv(name);
  slot->name = name;
  slot->had = current != nullptr;
  slot->old[0] = '\0';
  if (slot->had) {
    std::snprintf(slot->old, sizeof slot->old, "%s", current);
  }
  if (value) {
    gcu_env_set(name, value);
  } else {
    gcu_env_unset(name);
  }
}

void env_restore(EnvSlot * slot) {
  if (!slot->name) {
    return;
  }
  if (slot->had) {
    gcu_env_set(slot->name, slot->old);
  } else {
    gcu_env_unset(slot->name);
  }
}

bool run_isolated(void (*fn)(IsolateReport *)) {
  int fds[2];
  if (pipe(fds) != 0) {
    ADD_FAILURE() << "pipe failed";
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    ADD_FAILURE() << "fork failed";
    return false;
  }
  if (pid == 0) {
    IsolateReport report;
    std::memset(&report, 0, sizeof report);
    report.ok = 1;
    close(fds[0]);
    if (enter_namespace(&report)) {
      fn(&report);
    }
    if (write(fds[1], &report, sizeof report) != (ssize_t)sizeof report) {
      _exit(1);
    }
    _exit(0);
  }
  close(fds[1]);
  IsolateReport report;
  std::memset(&report, 0, sizeof report);
  size_t got = 0;
  while (got < sizeof report) {
    ssize_t n = read(fds[0], reinterpret_cast<char *>(&report) + got,
        sizeof report - got);
    if (n <= 0) {
      break;
    }
    got += (size_t)n;
  }
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (got != sizeof report || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    ADD_FAILURE() << "isolated scan did not report";
    return false;
  }
  if (!report.ok) {
    ADD_FAILURE() << report.message;
    return false;
  }
  return true;
}

struct IsolateCtx {
  std::string fixture;
  std::string basic;
  bool home_fallback;
};

IsolateCtx * g_ctx = nullptr;

void child_scan_only(IsolateReport * report) {
  if (!copy_file(g_ctx->basic, "/usr/share/fonts/canary.ttf")) {
    report_fail(report, "copying the canary failed");
    return;
  }
  Set held;
  GFNT_Error error;
  if (scan_one(g_ctx->fixture, nullptr, 0, &held.set, &error) != GFNT_OK) {
    report_fail(report, "scan of the fixture failed");
    return;
  }
  if (!find_ending(held.set, "/fixture.ttf")) {
    report_fail(report, "the fixture font was not listed");
    return;
  }
  if (count_containing(held.set, "/usr/share/fonts") != 0) {
    report_fail(report, "scan opened /usr/share/fonts");
  }
}

void child_system(IsolateReport * report) {
  if (!copy_file(g_ctx->basic, "/usr/share/fonts/canary.ttf")) {
    report_fail(report, "copying the canary failed");
    return;
  }
  EnvSlot home;
  EnvSlot xdg_home;
  EnvSlot xdg_dirs;
  std::memset(&home, 0, sizeof home);
  std::memset(&xdg_home, 0, sizeof xdg_home);
  std::memset(&xdg_dirs, 0, sizeof xdg_dirs);
  std::string home_dir = g_ctx->fixture + "/home";
  std::string xdg_dir = g_ctx->fixture + "/xdg-home";
  std::string data_dir = g_ctx->fixture + "/xdg-data";
  env_set(&home, "HOME", home_dir.c_str());
  if (g_ctx->home_fallback) {
    env_set(&xdg_home, "XDG_DATA_HOME", nullptr);
  } else {
    env_set(&xdg_home, "XDG_DATA_HOME", xdg_dir.c_str());
  }
  env_set(&xdg_dirs, "XDG_DATA_DIRS", data_dir.c_str());

  Set held;
  GFNT_Error error;
  GFNT_Result result = gfnt_fontset_scan_system(nullptr, 0, nullptr, &held.set,
      &error);
  env_restore(&home);
  env_restore(&xdg_home);
  env_restore(&xdg_dirs);
  if (result != GFNT_OK) {
    report_fail(report, "scan_system failed");
    return;
  }
  if (g_ctx->home_fallback) {
    if (!find_ending(held.set, "/.local/share/fonts/local.ttf")) {
      report_fail(report, "HOME/.local/share/fonts was not scanned");
      return;
    }
  } else if (count_containing(held.set, "/.local/share/fonts") != 0) {
    report_fail(report, "XDG_DATA_HOME was set and HOME/.local was scanned");
    return;
  }
  if (!g_ctx->home_fallback && !find_ending(held.set, "/xdg-home/fonts/xdg.ttf")) {
    report_fail(report, "XDG_DATA_HOME/fonts was not scanned");
    return;
  }
  if (!find_ending(held.set, "/xdg-data/fonts/data.ttf")) {
    report_fail(report, "XDG_DATA_DIRS entry was not scanned");
    return;
  }
  if (count_containing(held.set, "/.fonts") != 0) {
    report_fail(report, "absent ~/.fonts was treated as a font directory");
    return;
  }
  if (count_containing(held.set, "/usr/share/fonts") != 1
      || !find_ending(held.set, "/usr/share/fonts/canary.ttf")) {
    report_fail(report, "the host font directory was scanned");
    return;
  }
  for (size_t i = 0; i < gfnt_fontset_skip_count(held.set); i++) {
    if (std::strstr(gfnt_fontset_skip_at(held.set, i)->path, "/.fonts")) {
      report_fail(report, "absent ~/.fonts was reported as a skip");
      return;
    }
  }
}

void child_dot_fonts(IsolateReport * report) {
  EnvSlot home;
  EnvSlot xdg_home;
  EnvSlot xdg_dirs;
  std::memset(&home, 0, sizeof home);
  std::memset(&xdg_home, 0, sizeof xdg_home);
  std::memset(&xdg_dirs, 0, sizeof xdg_dirs);
  env_set(&home, "HOME", (g_ctx->fixture + "/home").c_str());
  env_set(&xdg_home, "XDG_DATA_HOME", nullptr);
  env_set(&xdg_dirs, "XDG_DATA_DIRS", (g_ctx->fixture + "/missing").c_str());
  Set held;
  GFNT_Error error;
  GFNT_Result result = gfnt_fontset_scan_system(nullptr, 0, nullptr, &held.set,
      &error);
  env_restore(&home);
  env_restore(&xdg_home);
  env_restore(&xdg_dirs);
  if (result != GFNT_OK || !find_ending(held.set, "/.fonts/dot.ttf")) {
    report_fail(report, "a face only under ~/.fonts was not listed");
  }
}

void child_default_data_dirs(IsolateReport * report) {
  if (!copy_file(g_ctx->basic, "/usr/local/share/fonts/share.ttf")) {
    report_fail(report, "copying into /usr/local/share/fonts failed");
    return;
  }
  EnvSlot home;
  EnvSlot xdg_home;
  EnvSlot xdg_dirs;
  std::memset(&home, 0, sizeof home);
  std::memset(&xdg_home, 0, sizeof xdg_home);
  std::memset(&xdg_dirs, 0, sizeof xdg_dirs);
  env_set(&home, "HOME", (g_ctx->fixture + "/home").c_str());
  env_set(&xdg_home, "XDG_DATA_HOME", nullptr);
  env_set(&xdg_dirs, "XDG_DATA_DIRS", nullptr);
  Set held;
  GFNT_Error error;
  GFNT_Result result = gfnt_fontset_scan_system(nullptr, 0, nullptr, &held.set,
      &error);
  env_restore(&home);
  env_restore(&xdg_home);
  env_restore(&xdg_dirs);
  if (result != GFNT_OK
      || !find_ending(held.set, "/usr/local/share/fonts/share.ttf")) {
    report_fail(report, "the default XDG_DATA_DIRS entry was not scanned");
  }
}

void child_two_data_dirs(IsolateReport * report) {
  EnvSlot home;
  EnvSlot xdg_home;
  EnvSlot xdg_dirs;
  std::memset(&home, 0, sizeof home);
  std::memset(&xdg_home, 0, sizeof xdg_home);
  std::memset(&xdg_dirs, 0, sizeof xdg_dirs);
  std::string spec = g_ctx->fixture + "/xdg-a:" + g_ctx->fixture + "/xdg-b";
  env_set(&home, "HOME", (g_ctx->fixture + "/home").c_str());
  env_set(&xdg_home, "XDG_DATA_HOME", nullptr);
  env_set(&xdg_dirs, "XDG_DATA_DIRS", spec.c_str());
  Set held;
  GFNT_Error error;
  GFNT_Result result = gfnt_fontset_scan_system(nullptr, 0, nullptr, &held.set,
      &error);
  env_restore(&home);
  env_restore(&xdg_home);
  env_restore(&xdg_dirs);
  if (result != GFNT_OK || !find_ending(held.set, "/xdg-a/fonts/a.ttf")
      || !find_ending(held.set, "/xdg-b/fonts/b.ttf")) {
    report_fail(report, "a colon-separated XDG_DATA_DIRS missed a face");
  }
}

void child_data_dirs_replace(IsolateReport * report) {
  if (!copy_file(g_ctx->basic, "/usr/local/share/fonts/share.ttf")) {
    report_fail(report, "copying into /usr/local/share/fonts failed");
    return;
  }
  EnvSlot home;
  EnvSlot xdg_home;
  EnvSlot xdg_dirs;
  std::memset(&home, 0, sizeof home);
  std::memset(&xdg_home, 0, sizeof xdg_home);
  std::memset(&xdg_dirs, 0, sizeof xdg_dirs);
  env_set(&home, "HOME", (g_ctx->fixture + "/home").c_str());
  env_set(&xdg_home, "XDG_DATA_HOME", nullptr);
  env_set(&xdg_dirs, "XDG_DATA_DIRS", (g_ctx->fixture + "/xdg-data").c_str());
  Set held;
  GFNT_Error error;
  GFNT_Result result = gfnt_fontset_scan_system(nullptr, 0, nullptr, &held.set,
      &error);
  env_restore(&home);
  env_restore(&xdg_home);
  env_restore(&xdg_dirs);
  if (result != GFNT_OK || !find_ending(held.set, "/xdg-data/fonts/data.ttf")
      || find_ending(held.set, "/usr/local/share/fonts/share.ttf")) {
    report_fail(report, "XDG_DATA_DIRS did not replace the default");
  }
}

void child_empty_system(IsolateReport * report) {
  EnvSlot home;
  EnvSlot xdg_home;
  EnvSlot xdg_dirs;
  std::memset(&home, 0, sizeof home);
  std::memset(&xdg_home, 0, sizeof xdg_home);
  std::memset(&xdg_dirs, 0, sizeof xdg_dirs);
  env_set(&home, "HOME", (g_ctx->fixture + "/home").c_str());
  env_set(&xdg_home, "XDG_DATA_HOME", nullptr);
  env_set(&xdg_dirs, "XDG_DATA_DIRS", (g_ctx->fixture + "/missing").c_str());
  Set held;
  GFNT_Error error;
  GFNT_Result result = gfnt_fontset_scan_system(nullptr, 0, nullptr, &held.set,
      &error);
  env_restore(&home);
  env_restore(&xdg_home);
  env_restore(&xdg_dirs);
  if (result != GFNT_OK || !held.set || gfnt_fontset_count(held.set) != 0) {
    report_fail(report, "an empty system list did not scan as empty");
  }
}

#endif

} // namespace

TEST(Discover, OneFont) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"), tree.child("basic.ttf")));
  Set held;
  GFNT_Error error;
  ASSERT_EQ(scan_one(tree.path(), nullptr, 0, &held.set, &error), GFNT_OK);
  ASSERT_EQ(gfnt_fontset_count(held.set), 1u);
  ASSERT_EQ(gfnt_fontset_skip_count(held.set), 0u);
  const GFNT_FontRecord * rec = gfnt_fontset_at(held.set, 0);
  ASSERT_NE(rec, nullptr);
  EXPECT_STREQ(rec->family, "Ghoti Fixture Basic");
  EXPECT_STREQ(rec->style, "Regular");
  EXPECT_EQ(rec->weight, 400u);
  EXPECT_EQ(rec->width, 5u);
  EXPECT_EQ(rec->slant, GFNT_SLANT_ROMAN);
  EXPECT_EQ(rec->unicode_range[0], 0x00000001u);
  EXPECT_EQ(rec->unicode_range[1], 0u);
  EXPECT_EQ(rec->unicode_range[2], 0u);
  EXPECT_EQ(rec->unicode_range[3], 0u);
  EXPECT_EQ(rec->flavour, GFNT_FLAVOUR_TRUETYPE);
  EXPECT_EQ(rec->index, 0u);
  EXPECT_EQ(std::strstr(rec->path, "/usr/share/fonts"), nullptr) << rec->path;
  EXPECT_NE(find_ending(held.set, "/basic.ttf"), nullptr);
}

TEST(Discover, Collection) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/collection.ttc"),
      tree.child("collection.ttc")));
  Set held;
  GFNT_Error error;
  ASSERT_EQ(scan_one(tree.path(), nullptr, 0, &held.set, &error), GFNT_OK);
  ASSERT_EQ(gfnt_fontset_count(held.set), 2u);
  const GFNT_FontRecord * first = find_ending(held.set, "/collection.ttc");
  ASSERT_NE(first, nullptr);
  const GFNT_FontRecord * second = nullptr;
  for (size_t i = 0; i < 2; i++) {
    const GFNT_FontRecord * rec = gfnt_fontset_at(held.set, i);
    if (rec->index == 0) {
      EXPECT_STREQ(rec->family, "Ghoti Fixture Collection 0");
    } else if (rec->index == 1) {
      EXPECT_STREQ(rec->family, "Ghoti Fixture Collection 1");
      second = rec;
    } else {
      ADD_FAILURE() << "unexpected index " << rec->index;
    }
  }
  EXPECT_NE(second, nullptr);
  EXPECT_EQ(gfnt_fontset_skip_count(held.set), 0u);
}

TEST(Discover, Nested) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(tree.mkdir_p("sub"));
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"),
      tree.child("sub/basic.ttf")));
  Set held;
  GFNT_Error error;
  ASSERT_EQ(scan_one(tree.path(), nullptr, 0, &held.set, &error), GFNT_OK);
  EXPECT_NE(find_ending(held.set, "/sub/basic.ttf"), nullptr);
  EXPECT_EQ(gfnt_fontset_count(held.set), 1u);
}

TEST(Discover, GzipBitmap) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/bitmap-gz.pcf.gz"),
      tree.child("bitmap.pcf.gz")));
  Set held;
  GFNT_Error error;
  ASSERT_EQ(scan_one(tree.path(), nullptr, 0, &held.set, &error), GFNT_OK);
  ASSERT_EQ(gfnt_fontset_count(held.set), 1u);
  const GFNT_FontRecord * rec = gfnt_fontset_at(held.set, 0);
  EXPECT_STREQ(rec->family, "Ghoti Fixture Bitmap");
  EXPECT_STREQ(rec->style, "Medium");
  EXPECT_EQ(rec->weight, 500u);
  EXPECT_EQ(rec->width, 5u);
  EXPECT_EQ(rec->slant, GFNT_SLANT_ROMAN);
  EXPECT_EQ(rec->flavour, GFNT_FLAVOUR_PCF);
  EXPECT_EQ(rec->index, 0u);
  EXPECT_EQ(rec->unicode_range[0], 0u);
  EXPECT_EQ(rec->unicode_range[1], 0u);
  EXPECT_EQ(rec->unicode_range[2], 0u);
  EXPECT_EQ(rec->unicode_range[3], 0u);
}

TEST(Discover, NotAFont) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"), tree.child("basic.ttf")));
  ASSERT_TRUE(write_text(tree.child("notes.txt"), "this is not a font\n"));
  Set held;
  GFNT_Error error;
  ASSERT_EQ(scan_one(tree.path(), nullptr, 0, &held.set, &error), GFNT_OK);
  EXPECT_EQ(gfnt_fontset_count(held.set), 1u);
  EXPECT_EQ(gfnt_fontset_skip_count(held.set), 0u);
  EXPECT_EQ(find_skip_ending(held.set, "/notes.txt"), nullptr);
  EXPECT_NE(find_ending(held.set, "/basic.ttf"), nullptr);
}

TEST(Discover, Unreadable) {
  if (geteuid() == 0) {
    GTEST_SKIP() << "a mode 0 file is still readable as root";
  }
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"), tree.child("basic.ttf")));
  std::string hidden = tree.child("hidden.ttf");
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"), hidden));
  ASSERT_EQ(chmod(hidden.c_str(), 0), 0);
  Set held;
  GFNT_Error error;
  ASSERT_EQ(scan_one(tree.path(), nullptr, 0, &held.set, &error), GFNT_OK);
  EXPECT_NE(find_ending(held.set, "/basic.ttf"), nullptr);
  const GFNT_FontSkip * skip = find_skip_ending(held.set, "/hidden.ttf");
  ASSERT_NE(skip, nullptr);
  EXPECT_EQ(skip->result, GFNT_ERR_IO);
  EXPECT_EQ(count_containing(held.set, "/hidden.ttf"), 0u);
  chmod(hidden.c_str(), 0600);
}

TEST(Discover, CorruptAndLimitKeepTheFacesAlreadyListed) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"), tree.child("basic.ttf")));
  // A collection header that claims more faces than the file can hold.
  const unsigned char corrupt[] = {
    't', 't', 'c', 'f', 0x00, 0x01, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
  };
  ASSERT_TRUE(write_bytes(tree.child("corrupt.ttc"), corrupt, sizeof corrupt));
  // An sfnt whose numTables exceeds GFNT_Limits::max_tables (512).
  unsigned char limited[12] = {
    0x00, 0x01, 0x00, 0x00, 0x02, 0x58, 0, 0, 0, 0, 0, 0,
  };
  ASSERT_TRUE(write_bytes(tree.child("limited.ttf"), limited, sizeof limited));
  Set held;
  GFNT_Error error;
  ASSERT_EQ(scan_one(tree.path(), nullptr, 0, &held.set, &error), GFNT_OK);
  EXPECT_NE(find_ending(held.set, "/basic.ttf"), nullptr);
  EXPECT_EQ(count_containing(held.set, "/corrupt.ttc"), 0u);
  EXPECT_EQ(count_containing(held.set, "/limited.ttf"), 0u);
  const GFNT_FontSkip * corrupt_skip = find_skip_ending(held.set, "/corrupt.ttc");
  const GFNT_FontSkip * limit_skip = find_skip_ending(held.set, "/limited.ttf");
  ASSERT_NE(corrupt_skip, nullptr);
  ASSERT_NE(limit_skip, nullptr);
  EXPECT_EQ(corrupt_skip->result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(limit_skip->result, GFNT_ERR_LIMIT);
}

TEST(Discover, MissingRoot) {
  GFNT_FontSet * set = nullptr;
  GFNT_Error error;
  const char * missing[] = { "/nonexistent/ghoti.io/font-discover" };
  EXPECT_EQ(gfnt_fontset_scan(missing, 1, nullptr, 0, nullptr, &set, &error),
      GFNT_ERR_IO);
  EXPECT_EQ(set, nullptr);
  EXPECT_EQ(error.result, GFNT_ERR_IO);
}

TEST(Discover, Empty) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  Set held;
  GFNT_Error error;
  ASSERT_EQ(scan_one(tree.path(), nullptr, 0, &held.set, &error), GFNT_OK);
  EXPECT_EQ(gfnt_fontset_count(held.set), 0u);
  EXPECT_EQ(gfnt_fontset_skip_count(held.set), 0u);
}

TEST(Discover, Cycle) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(tree.mkdir_p("scan"));
  ASSERT_TRUE(tree.mkdir_p("outside"));
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"),
      tree.child("outside/basic.ttf")));
  ASSERT_EQ(symlink("../outside/basic.ttf", tree.child("scan/link.ttf").c_str()),
      0);
  ASSERT_EQ(symlink(".", tree.child("scan/loop").c_str()), 0);
  std::string scan = tree.child("scan");
  std::string link = scan + "/also";
  ASSERT_EQ(symlink(scan.c_str(), link.c_str()), 0);
  Set held;
  GFNT_Error error;
  const char * dirs[] = { scan.c_str(), link.c_str() };
  ASSERT_EQ(gfnt_fontset_scan(dirs, 2, nullptr, 0, nullptr, &held.set, &error),
      GFNT_OK);
  EXPECT_EQ(gfnt_fontset_count(held.set), 1u);
  EXPECT_NE(find_ending(held.set, "/link.ttf"), nullptr);
}

TEST(Discover, DeeperThan64IsASkip) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  std::string at = tree.path();
  for (int i = 0; i < 64; i++) {
    at += "/d";
    ASSERT_EQ(gcu_dir_create(at.c_str()), GCU_FILE_OK) << at;
  }
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"), at + "/kept.ttf"));
  std::string too_deep = at + "/d";
  ASSERT_EQ(gcu_dir_create(too_deep.c_str()), GCU_FILE_OK);
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"), too_deep + "/dropped.ttf"));
  Set held;
  GFNT_Error error;
  ASSERT_EQ(scan_one(tree.path(), nullptr, 0, &held.set, &error), GFNT_OK);
  EXPECT_NE(find_ending(held.set, "/kept.ttf"), nullptr);
  EXPECT_EQ(find_ending(held.set, "/dropped.ttf"), nullptr);
  const GFNT_FontSkip * skip = find_skip_ending(held.set, "/d/d");
  ASSERT_NE(skip, nullptr);
  EXPECT_EQ(skip->result, GFNT_ERR_LIMIT);
}

TEST(Discover, FontsDirFillsOnlyWhatTheFaceDidNotState) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/bitmap.bdf"), tree.child("bitmap.bdf")));
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/bitmap.psf"), tree.child("console.psf")));
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/bitmap.psf"), tree.child("italic.psf")));
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"), tree.child("basic.ttf")));
  ASSERT_TRUE(write_text(tree.child("fonts.dir"),
      "4\n"
      "bitmap.bdf -misc-Override-Black-I-Expanded--0-0-0-0-p-0-iso8859-1\n"
      "console.psf -misc-Fixed-Bold-O-Condensed--16-160-75-75-c-80-iso8859-1\n"
      "italic.psf -misc-Fixed-medium-I-normal--16-160-75-75-c-80-iso8859-1\n"
      "ghost.pcf -misc-Nope-medium-r-normal--0-0-0-0-p-0-iso8859-1\n"));
  ASSERT_TRUE(write_text(tree.child("fonts.scale"), "1\nnot-a-font.pcf\n"));
  Set held;
  GFNT_Error error;
  ASSERT_EQ(scan_one(tree.path(), nullptr, 0, &held.set, &error), GFNT_OK);
  EXPECT_EQ(find_ending(held.set, "/ghost.pcf"), nullptr);
  EXPECT_EQ(find_skip_ending(held.set, "/fonts.scale"), nullptr);
  const GFNT_FontRecord * bdf = find_ending(held.set, "/bitmap.bdf");
  const GFNT_FontRecord * psf = find_ending(held.set, "/console.psf");
  const GFNT_FontRecord * italic = find_ending(held.set, "/italic.psf");
  const GFNT_FontRecord * basic = find_ending(held.set, "/basic.ttf");
  ASSERT_NE(bdf, nullptr);
  ASSERT_NE(psf, nullptr);
  ASSERT_NE(italic, nullptr);
  ASSERT_NE(basic, nullptr);
  EXPECT_STREQ(bdf->family, "Ghoti Fixture Bitmap");
  EXPECT_EQ(bdf->weight, 500u);
  EXPECT_EQ(bdf->width, 5u);
  EXPECT_EQ(bdf->slant, GFNT_SLANT_ROMAN);
  EXPECT_STREQ(psf->family, "");
  EXPECT_STREQ(psf->style, "");
  EXPECT_EQ(psf->weight, 700u);
  EXPECT_EQ(psf->width, 3u);
  EXPECT_EQ(psf->slant, GFNT_SLANT_OBLIQUE);
  EXPECT_EQ(italic->slant, GFNT_SLANT_ITALIC);
  EXPECT_EQ(basic->weight, 400u);
  EXPECT_EQ(gfnt_fontset_count(held.set), 4u);
}

TEST(Discover, Alias) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"), tree.child("basic.ttf")));
  ASSERT_TRUE(write_text(tree.child("fonts.alias"),
      "sans -misc-should-lose-medium-r-normal--0-0-0-0-p-0-iso8859-1\n"
      "serif -adobe-times-medium-r-normal--0-0-0-0-p-0-iso8859-1\n"
      "mono basic.ttf\n"));
  GFNT_FontAlias caller[] = {
    { "sans", "Caller Sans" },
    { "courier", "-adobe-courier-medium-r-normal--0-0-0-0-p-0-iso8859-1" },
  };
  Set held;
  GFNT_Error error;
  ASSERT_EQ(scan_one(tree.path(), caller, 2, &held.set, &error), GFNT_OK);
  const char * resolved = nullptr;
  ASSERT_EQ(gfnt_fontset_resolve_alias(held.set, "sans", &resolved, &error),
      GFNT_OK);
  EXPECT_STREQ(resolved, "Caller Sans");
  ASSERT_EQ(gfnt_fontset_resolve_alias(held.set, "serif", &resolved, &error),
      GFNT_OK);
  EXPECT_STREQ(resolved, "times");
  ASSERT_EQ(gfnt_fontset_resolve_alias(held.set, "mono", &resolved, &error),
      GFNT_OK);
  EXPECT_STREQ(resolved, "basic.ttf");
  ASSERT_EQ(gfnt_fontset_resolve_alias(held.set, "courier", &resolved, &error),
      GFNT_OK);
  EXPECT_STREQ(resolved, "courier");
  const char * missing = "not-a-name";
  ASSERT_EQ(gfnt_fontset_resolve_alias(held.set, missing, &resolved, &error),
      GFNT_OK);
  EXPECT_EQ(resolved, missing);
  bool saw_sans = false;
  for (size_t i = 0; i < gfnt_fontset_alias_count(held.set); i++) {
    const GFNT_FontAlias * alias = gfnt_fontset_alias_at(held.set, i);
    if (std::strcmp(alias->name, "sans") == 0) {
      saw_sans = true;
      EXPECT_STREQ(alias->target, "Caller Sans");
    }
  }
  EXPECT_TRUE(saw_sans);
}

TEST(Discover, AliasDepth) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  // Seven steps still resolve. The eighth step is GFNT_ERR_LIMIT.
  GFNT_FontAlias aliases[] = {
    { "a0", "a1" }, { "a1", "a2" }, { "a2", "a3" }, { "a3", "a4" },
    { "a4", "a5" }, { "a5", "a6" }, { "a6", "done" },
    { "b0", "b1" }, { "b1", "b2" }, { "b2", "b3" }, { "b3", "b4" },
    { "b4", "b5" }, { "b5", "b6" }, { "b6", "b7" }, { "b7", "too-far" },
    { "c", "d" }, { "d", "c" },
  };
  Set held;
  GFNT_Error error;
  ASSERT_EQ(scan_one(tree.path(), aliases, sizeof aliases / sizeof aliases[0],
                &held.set, &error),
      GFNT_OK);
  const char * resolved = nullptr;
  ASSERT_EQ(gfnt_fontset_resolve_alias(held.set, "a0", &resolved, &error),
      GFNT_OK);
  EXPECT_STREQ(resolved, "done");
  EXPECT_EQ(gfnt_fontset_resolve_alias(held.set, "b0", &resolved, &error),
      GFNT_ERR_LIMIT);
  EXPECT_EQ(error.result, GFNT_ERR_LIMIT);
  EXPECT_EQ(gfnt_fontset_resolve_alias(held.set, "c", &resolved, &error),
      GFNT_ERR_LIMIT);
}

TEST(Discover, Os2ItalicWinsOverOblique) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  std::string path = tree.child("both.ttf");
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"), path));
  ASSERT_TRUE(patch_fs_selection(path, 0x0201u));
  Set held;
  GFNT_Error error;
  ASSERT_EQ(scan_one(tree.path(), nullptr, 0, &held.set, &error), GFNT_OK);
  const GFNT_FontRecord * rec = find_ending(held.set, "/both.ttf");
  ASSERT_NE(rec, nullptr);
  EXPECT_EQ(rec->slant, GFNT_SLANT_ITALIC);
}

TEST(Discover, BadArgs) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  GFNT_FontSet * set = nullptr;
  GFNT_Error error;
  const char * dirs[] = { tree.path().c_str() };
  EXPECT_EQ(gfnt_fontset_scan(nullptr, 1, nullptr, 0, nullptr, &set, &error),
      GFNT_ERR_INVALID);
  EXPECT_EQ(set, nullptr);
  EXPECT_EQ(gfnt_fontset_scan(dirs, 1, nullptr, 0, nullptr, nullptr, &error),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_fontset_scan_system(nullptr, 0, nullptr, nullptr, &error),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_fontset_count(nullptr), 0u);
  EXPECT_EQ(gfnt_fontset_at(nullptr, 0), nullptr);
  gfnt_fontset_free(nullptr);
}

#if defined(__linux__)

TEST(Discover, ScanDoesNotOpenHostFonts) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(tree.mkdir_p("scan"));
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"),
      tree.child("scan/fixture.ttf")));
  IsolateCtx ctx;
  ctx.fixture = tree.child("scan");
  ctx.basic = gfnttest::data("fonts/basic.ttf");
  ctx.home_fallback = false;
  g_ctx = &ctx;
  EXPECT_TRUE(run_isolated(child_scan_only));
  g_ctx = nullptr;
}

TEST(Discover, System) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(tree.mkdir_p("home/.local/share/fonts"));
  ASSERT_TRUE(tree.mkdir_p("xdg-home/fonts"));
  ASSERT_TRUE(tree.mkdir_p("xdg-data/fonts"));
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"),
      tree.child("home/.local/share/fonts/local.ttf")));
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/collection.ttc"),
      tree.child("xdg-home/fonts/xdg.ttf")));
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/bitmap.bdf"),
      tree.child("xdg-data/fonts/data.ttf")));
  IsolateCtx ctx;
  ctx.fixture = tree.path();
  ctx.basic = gfnttest::data("fonts/basic.ttf");
  ctx.home_fallback = false;
  g_ctx = &ctx;
  EXPECT_TRUE(run_isolated(child_system));
  g_ctx = nullptr;
}

TEST(Discover, SystemUsesHomeWhenXdgDataHomeIsUnset) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(tree.mkdir_p("home/.local/share/fonts"));
  ASSERT_TRUE(tree.mkdir_p("xdg-data/fonts"));
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"),
      tree.child("home/.local/share/fonts/local.ttf")));
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/bitmap.bdf"),
      tree.child("xdg-data/fonts/data.ttf")));
  IsolateCtx ctx;
  ctx.fixture = tree.path();
  ctx.basic = gfnttest::data("fonts/basic.ttf");
  ctx.home_fallback = true;
  g_ctx = &ctx;
  EXPECT_TRUE(run_isolated(child_system));
  g_ctx = nullptr;
}

TEST(Discover, SystemListsHomeDotFonts) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(tree.mkdir_p("home/.fonts"));
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"),
      tree.child("home/.fonts/dot.ttf")));
  IsolateCtx ctx;
  ctx.fixture = tree.path();
  ctx.basic = gfnttest::data("fonts/basic.ttf");
  ctx.home_fallback = false;
  g_ctx = &ctx;
  EXPECT_TRUE(run_isolated(child_dot_fonts));
  g_ctx = nullptr;
}

TEST(Discover, SystemDefaultDataDirs) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(tree.mkdir_p("home"));
  IsolateCtx ctx;
  ctx.fixture = tree.path();
  ctx.basic = gfnttest::data("fonts/basic.ttf");
  ctx.home_fallback = false;
  g_ctx = &ctx;
  EXPECT_TRUE(run_isolated(child_default_data_dirs));
  g_ctx = nullptr;
}

TEST(Discover, SystemTwoDataDirs) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(tree.mkdir_p("home"));
  ASSERT_TRUE(tree.mkdir_p("xdg-a/fonts"));
  ASSERT_TRUE(tree.mkdir_p("xdg-b/fonts"));
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/basic.ttf"),
      tree.child("xdg-a/fonts/a.ttf")));
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/bitmap.bdf"),
      tree.child("xdg-b/fonts/b.ttf")));
  IsolateCtx ctx;
  ctx.fixture = tree.path();
  ctx.basic = gfnttest::data("fonts/basic.ttf");
  ctx.home_fallback = false;
  g_ctx = &ctx;
  EXPECT_TRUE(run_isolated(child_two_data_dirs));
  g_ctx = nullptr;
}

TEST(Discover, SystemDataDirsReplaceDefault) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(tree.mkdir_p("home"));
  ASSERT_TRUE(tree.mkdir_p("xdg-data/fonts"));
  ASSERT_TRUE(copy_file(gfnttest::data("fonts/bitmap.bdf"),
      tree.child("xdg-data/fonts/data.ttf")));
  IsolateCtx ctx;
  ctx.fixture = tree.path();
  ctx.basic = gfnttest::data("fonts/basic.ttf");
  ctx.home_fallback = false;
  g_ctx = &ctx;
  EXPECT_TRUE(run_isolated(child_data_dirs_replace));
  g_ctx = nullptr;
}

TEST(Discover, SystemEmptyWhenDirectoriesAreAbsent) {
  TempTree tree;
  ASSERT_TRUE(tree.ok());
  ASSERT_TRUE(tree.mkdir_p("home"));
  IsolateCtx ctx;
  ctx.fixture = tree.path();
  ctx.basic = gfnttest::data("fonts/basic.ttf");
  ctx.home_fallback = false;
  g_ctx = &ctx;
  EXPECT_TRUE(run_isolated(child_empty_system));
  g_ctx = nullptr;
}

#endif

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
