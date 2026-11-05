// lhsyncd's Pico 4 side (src/headset/headset.h): Android properties (through getprop: this lhsyncd is a static musl
// binary, built without the NDK), the factory camera calibration in /mnt/vendor/persist/pvr/mav0 (Kalibr-style
// sensor.yaml per camera: omnidirectional model + radtan distortion, extrinsics against the IMU), and lhsight as a
// Frida script (frida-inject + lhsight.js, the headset's cameras filled in) inside pvrtrackingservice: it reads the
// camera rings and writes nothing.
//
// What pvrtrackingservice holds (Pico 4 A8110, PUI 5.13): the four tracking cameras' frames, 640x480, 16-bit words
// with 10-bit pixels, in 4 rings of 8 dmabufs of w*h*2 + one page, allocated camera after camera. The cameras run
// together at 60 frames/s, alternating two exposures.
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "headset.h"

#define TRACKER "pvrtrackingservice"
#define RUNDIR "/dev/.questlhsync"
#define LOGFILE RUNDIR "/lhsyncd.log"
#define PAGE 4096

const char *const hs_capture_name = "frida";
static const char *moddir = "/data/adb/modules/questlhsync";
static const char *const mav0_dirs[] = {"/mnt/vendor/persist/pvr/mav0", "/data/misc/pxr/persist/pvr/mav0"};

void hs_log(const char *line) {
  struct stat st;
  if (!stat(LOGFILE, &st) && st.st_size > (1 << 20)) rename(LOGFILE, LOGFILE ".1");
  FILE *f = fopen(LOGFILE, "a");
  if (!f) return;
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  char ts[32];
  strftime(ts, sizeof ts, "%m-%d %H:%M:%S", &tm);
  fprintf(f, "%s %s\n", ts, line);
  fclose(f);
}

static void getprop(const char *name, char *out, size_t n) {
  char cmd[128];
  out[0] = 0;
  snprintf(cmd, sizeof cmd, "/system/bin/getprop %s 2>/dev/null", name);
  FILE *p = popen(cmd, "r");
  if (!p) return;
  if (fgets(out, (int)n, p)) out[strcspn(out, "\r\n")] = 0;
  pclose(p);
}

void hs_device_info(char *serial, char *model, char *fw, size_t n) {
  char dev[128];
  getprop("ro.serialno", serial, n);
  getprop("ro.product.device", dev, sizeof dev);
  getprop("ro.build.display.id", fw, n);
  if (!strcmp(dev, "PICOA8110")) snprintf(model, n, "Pico 4");
  else {
    char m[96];
    getprop("ro.product.model", m, sizeof m);
    snprintf(model, n, "Pico %s", m[0] ? m : dev);
  }
}

// a whole file, NUL-terminated (malloc'd), or NULL
static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc(n > 0 ? n + 1 : 1);
  size_t got = n > 0 ? fread(buf, 1, n, f) : 0;
  fclose(f);
  buf[got] = 0;
  if (len) *len = got;
  return buf;
}

// ---------------------------------------------------------------- the calibration (sensor.yaml per camera)
struct cam {
  int id, w, h;
  double in[5];    // xi fu fv pu pv
  double dist[4];  // k1 k2 p1 p2
  double T[16];    // body (IMU) <- camera, row-major
};

// the numbers in the [...] after "key:", up to max; how many
static int ynums(const char *y, const char *key, double *out, int max) {
  const char *p = strstr(y, key);
  if (!p) return 0;
  p = strchr(p, '[');
  if (!p) return 0;
  int n = 0;
  for (p++; n < max;) {
    char *e;
    double v = strtod(p, &e);
    if (e == p) break;
    out[n++] = v;
    for (p = e; *p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t'; p++) {}
    if (*p == ']') break;
  }
  return n;
}

static int yword(const char *y, const char *key, const char *want) {  // "key: want"
  const char *p = strstr(y, key);
  if (!p) return 0;
  for (p += strlen(key); *p == ' ' || *p == ':'; p++) {}
  return !strncmp(p, want, strlen(want));
}

// the tracking cameras: omni + radtan, small images (cam4 is the colour camera, cam7 another one)
static int load_cams(struct cam *c, int max, const char **dir_used) {
  for (size_t d = 0; d < sizeof mav0_dirs / sizeof *mav0_dirs; d++) {
    int n = 0;
    for (int i = 0; i < 8 && n < max; i++) {
      char path[256];
      snprintf(path, sizeof path, "%s/cam%d/sensor.yaml", mav0_dirs[d], i);
      char *y = read_file(path, NULL);
      if (!y) continue;
      double res[2], T[16];
      struct cam k = {.id = i};
      int ok = yword(y, "camera_model", "omni") && yword(y, "distortion_model", "radtan") &&
               ynums(y, "resolution:", res, 2) == 2 && ynums(y, "intrinsics:", k.in, 5) == 5 &&
               ynums(y, "distortion_coefficients:", k.dist, 4) == 4 && ynums(y, "data:", T, 16) == 16 &&
               res[0] > 0 && res[0] <= 1024 && res[1] > 0 && res[1] <= 1024;
      free(y);
      if (!ok) continue;
      k.w = (int)res[0];
      k.h = (int)res[1];
      memcpy(k.T, T, sizeof T);
      c[n++] = k;
    }
    if (n) { *dir_used = mav0_dirs[d]; return n; }
  }
  return 0;
}

int hs_calibration(char **data, size_t *len, char *name, size_t n) {
  struct cam c[8];
  const char *dir = "";
  int nc = load_cams(c, 8, &dir);
  if (!nc) { snprintf(name, n, "no tracking camera calibration in /mnt/vendor/persist/pvr/mav0"); return -1; }
  char dev[96];
  getprop("ro.product.device", dev, sizeof dev);
  size_t cap = 8192, o = 0;
  char *js = malloc(cap);
  o += snprintf(js + o, cap - o, "{\"kind\": \"pico\", \"device\": \"%s\", \"cameras\": [", dev);
  for (int i = 0; i < nc; i++) {
    o += snprintf(js + o, cap - o,
                  "%s\n {\"id\": %d, \"w\": %d, \"h\": %d, \"model\": \"omni\", \"intrinsics\": [%.12g, %.12g, %.12g, "
                  "%.12g, %.12g], \"distortion_model\": \"radtan\", \"distortion\": [%.12g, %.12g, %.12g, %.12g], "
                  "\"T_BS\": [",
                  i ? "," : "", c[i].id, c[i].w, c[i].h, c[i].in[0], c[i].in[1], c[i].in[2], c[i].in[3], c[i].in[4],
                  c[i].dist[0], c[i].dist[1], c[i].dist[2], c[i].dist[3]);
    for (int j = 0; j < 16; j++) o += snprintf(js + o, cap - o, "%s%.12g", j ? ", " : "", c[i].T[j]);
    o += snprintf(js + o, cap - o, "]}");
  }
  o += snprintf(js + o, cap - o, "\n]}\n");
  *data = js;
  *len = o;
  snprintf(name, n, "pico/mav0");
  (void)dir;
  return 0;
}

// "id:w:h:scan:bpp:size,..." for lhsight: every tracking camera, 16-bit, w*h*2 + a page
static int camera_list(char *out, size_t cap) {
  struct cam c[8];
  const char *dir;
  int n = load_cams(c, 8, &dir);
  size_t o = 0;
  out[0] = 0;
  for (int i = 0; i < n && o + 48 < cap; i++) {
    long size = ((long)c[i].w * c[i].h * 2 + PAGE - 1) / PAGE * PAGE + PAGE;
    o += snprintf(out + o, cap - o, "%s%d:%d:%d:1:2:%ld", i ? "," : "", c[i].id, c[i].w, c[i].h, size);
  }
  return n;
}

static pid_t tracker_pid(void) {
  DIR *d = opendir("/proc");
  if (!d) return 0;
  struct dirent *e;
  pid_t found = 0;
  while (!found && (e = readdir(d))) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    char p[64], cmd[256];
    snprintf(p, sizeof p, "/proc/%s/cmdline", e->d_name);
    int fd = open(p, O_RDONLY);
    if (fd < 0) continue;
    ssize_t n = read(fd, cmd, sizeof cmd - 1);
    close(fd);
    if (n <= 0) continue;
    cmd[n] = 0;
    const char *base = strrchr(cmd, '/') ? strrchr(cmd, '/') + 1 : cmd;
    if (!strcmp(base, TRACKER)) found = atoi(e->d_name);
  }
  closedir(d);
  return found;
}

// replaces each @KEY@ in js
static char *fill(const char *js, const char *key, const char *val) {
  size_t kl = strlen(key), vl = strlen(val), cnt = 0;
  for (const char *p = js; (p = strstr(p, key)); p += kl) cnt++;
  char *out = malloc(strlen(js) + cnt * vl + 1), *o = out;
  for (const char *p = js;;) {
    const char *q = strstr(p, key);
    if (!q) { strcpy(o, p); break; }
    memcpy(o, p, q - p);
    o += q - p;
    memcpy(o, val, vl);
    o += vl;
    p = q + kl;
  }
  return out;
}

// lhsight.js with this headset's cameras filled in, in the run dir: its path in out ("" on failure, why in note)
static void prepare_script(char *out, size_t cap, char *note, size_t ncap) {
  char src[512], cams[512];
  out[0] = 0;
  snprintf(src, sizeof src, "%s/lhsight.js", moddir);
  char *js = read_file(src, NULL);
  if (!js) { snprintf(note, ncap, "can't read %s", src); return; }
  if (!camera_list(cams, sizeof cams)) {
    snprintf(note, ncap, "no tracking cameras in the camera calibration");
    free(js);
    return;
  }
  snprintf(note, ncap, "tracking cameras (id:width:height:scan:bpp:size) %s", cams);
  char *a = fill(js, "@CAMS@", cams), *b = fill(a, "@PAIRS@", "1");
  free(js);
  free(a);
  snprintf(out, cap, "%s/lhsight.js", RUNDIR);
  FILE *f = fopen(out, "wb");
  if (!f) { snprintf(note, ncap, "can't write %s", out); out[0] = 0; free(b); return; }
  fputs(b, f);
  fclose(f);
  chmod(out, 0644);
  free(b);
}

// a frida-inject still running a lhsight.js would make the next start a second injection: stop those, and only those
static int is_lhsight(const char *pid) {
  char p[64], cmd[1024];
  snprintf(p, sizeof p, "/proc/%s/cmdline", pid);
  int fd = open(p, O_RDONLY);
  if (fd < 0) return 0;
  ssize_t len = read(fd, cmd, sizeof cmd - 1);
  close(fd);
  if (len <= 0) return 0;
  cmd[len] = 0;
  const char *base = strrchr(cmd, '/') ? strrchr(cmd, '/') + 1 : cmd;
  if (strcmp(base, "frida-inject")) return 0;
  for (ssize_t i = strlen(cmd) + 1; i < len; i += strlen(cmd + i) + 1) {
    size_t l = strlen(cmd + i);
    if (l >= 10 && !strcmp(cmd + i + l - 10, "lhsight.js")) return 1;
  }
  return 0;
}

static void kill_strays(void) {
  DIR *d = opendir("/proc");
  if (!d) return;
  struct dirent *e;
  char found[16][16];
  int n = 0;
  while (n < 16 && (e = readdir(d)))
    if (e->d_name[0] >= '0' && e->d_name[0] <= '9' && is_lhsight(e->d_name)) snprintf(found[n++], 16, "%s", e->d_name);
  closedir(d);
  for (int i = 0; i < n; i++) {
    char m[96];
    snprintf(m, sizeof m, "stopping a stray lhsight (frida-inject pid %s)", found[i]);
    hs_log(m);
    kill(atoi(found[i]), SIGTERM);
  }
  for (int t = 0; n && t < 30; t++) {
    usleep(100000);
    int left = 0;
    for (int i = 0; i < n; i++) left += is_lhsight(found[i]);
    if (!left) return;
  }
  for (int i = 0; i < n; i++)
    if (is_lhsight(found[i])) kill(atoi(found[i]), SIGKILL);
}

void hs_init(int argc, char **argv) {
  if (argc > 1) moddir = argv[1];
  mkdir(RUNDIR, 0755);
  kill_strays();
  char m[600];
  snprintf(m, sizeof m, "module %s", moddir);
  hs_log(m);
}

pid_t hs_capture_start(int out_fd, char *msg, size_t n) {
  pid_t tracker = tracker_pid();
  if (!tracker) { snprintf(msg, n, TRACKER " not running"); return 0; }
  char frida[512], script[512], pidstr[16], note[600];
  mkdir(RUNDIR, 0755);
  mkdir(RUNDIR "/tmp", 0755);
  prepare_script(script, sizeof script, note, sizeof note);
  if (!script[0]) { snprintf(msg, n, "%s", note); return 0; }
  snprintf(frida, sizeof frida, "%s/frida-inject", moddir);
  snprintf(pidstr, sizeof pidstr, "%d", tracker);
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    dup2(out_fd, 1);
    dup2(out_fd, 2);
    int nul = open("/dev/null", O_RDONLY);
    dup2(nul, 0);
    for (int fd = 3; fd < 1024; fd++) close(fd);
    setenv("TMPDIR", RUNDIR "/tmp", 1);  // frida's scratch files: tmpfs, never /data/local/tmp
    chdir(RUNDIR);
    execl(frida, "frida-inject", "-p", pidstr, "-s", script, (char *)NULL);
    _exit(127);
  }
  if (pid < 0) return -1;
  snprintf(msg, n, "lhsight started in " TRACKER " (pid %d); %s", tracker, note);
  return pid;
}
