// Standalone POSIX fixture: compile this TU only, with C++20, AURORA_H700,
// platform/include, SDL3 headers/link, and function/data sections + linker GC
// (-ffunction-sections -fdata-sections -Wl,--gc-sections on Linux). Including
// production exposes its private Copy/RedactedLines without a runtime selftest.
#if !defined(AURORA_H700) || defined(_WIN32) || defined(__ANDROID__)
#error This fixture requires the H700 POSIX log copier
#endif
#include "../platform/port_log_file.cpp"

#include <sys/wait.h>

namespace {
int sFailures = 0;
constexpr size_t kExpectedCap = 8 * 1024 * 1024;
constexpr char kHeader[] = "metroid_prime_port log, started fixture\n";
constexpr char kExpectedMarker[] = "\nport: log truncated at 8 MiB; further file output discarded\n";
constexpr char kPrivatePath[] = "/home/h700-log-fixture";
constexpr char kPrefix[] = "split /home/h700-";
constexpr char kSuffix[] = "log-fixture/disc.iso\n";
constexpr char kTail[] = "WRITER_REACHED_TAIL /home/h700-log-fixture/final";

bool Check(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++sFailures;
  }
  return ok;
}

// Unlike production WriteAll, report failures so the writer cannot pass without
// reaching its final bytes. SIGPIPE is deliberately default in that process.
bool Send(int fd, const char* data, size_t size) {
  while (size != 0) {
    const ssize_t n = write(fd, data, size);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    data += n;
    size -= static_cast< size_t >(n);
  }
  return true;
}

struct Fixture {
  int file = -1;
  int terminal = -1;
  int pipeEnds[2] = {-1, -1};

  bool Open() {
    char name[] = "/tmp/mp-h700-log-copy-XXXXXX";
    file = mkstemp(name);
    if (!Check(file >= 0, "create private temporary log")) {
      return false;
    }
    // Keep only the descriptor: no game/user files and no named flood artifact.
    if (!Check(unlink(name) == 0, "unlink private temporary log")) {
      return false;
    }
    terminal = open("/dev/null", O_WRONLY);
    return Check(terminal >= 0, "open discarded terminal output") &&
           Check(pipe(pipeEnds) == 0, "create game output pipe") &&
           Check(Send(file, kHeader, sizeof(kHeader) - 1), "write initial header before fork");
  }

  void ClosePipe() {
    for (int& fd : pipeEnds) {
      if (fd >= 0) {
        close(fd);
        fd = -1;
      }
    }
  }

  ~Fixture() {
    ClosePipe();
    if (terminal >= 0) close(terminal);
    if (file >= 0) close(file);
  }
};

bool Reap(pid_t child, const char* message) {
  int status = 0;
  pid_t result;
  do {
    result = waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);
  return Check(result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, message);
}

void Capture(void* context, const char* data, size_t size) {
  static_cast< std::string* >(context)->append(data, size);
}

void CheckSplitPrivacy() {
  PortLogRedact::Rules rules;
  rules.Add(kPrivatePath, "~", false);
  std::string output;
  PortLogFile::RedactedLines lines;
  lines.rules = &rules;
  lines.sink = Capture;
  lines.context = &output;
  // Deterministic read-boundary coverage: separate pipe writes below may be
  // coalesced by the kernel, whereas these calls force the path to be split.
  lines.Feed(kPrefix, sizeof(kPrefix) - 1);
  Check(output.empty(), "partial privacy path is buffered");
  lines.Feed(kSuffix, sizeof(kSuffix) - 1);
  Check(output == "split ~/disc.iso\n", "production line buffer redacts split privacy path");
}

void RunCopy(bool flood) {
  Fixture fixture;
  if (!fixture.Open()) {
    return;
  }
  PortLogFile::sRules = {};
  PortLogFile::sRules.Add(kPrivatePath, "~", false);
  long maxFd = sysconf(_SC_OPEN_MAX);
  maxFd = maxFd < 256 ? 256 : maxFd > 65536 ? 65536 : maxFd;
  const pid_t copier = fork();
  if (!Check(copier >= 0, "fork actual production copier")) {
    return;
  }
  if (copier == 0) {
    // Test-only deadlines bound a broken drain/EOF implementation without any
    // polling loop or changes to production signals/protocol. No flood capture.
    signal(SIGALRM, SIG_DFL);
    alarm(20);
    if (dup2(fixture.terminal, STDOUT_FILENO) < 0 || dup2(fixture.terminal, STDERR_FILENO) < 0) {
      _exit(2);
    }
    close(fixture.pipeEnds[1]);
    PortLogFile::Copy(fixture.pipeEnds[0], fixture.terminal, false, fixture.file, static_cast< int >(maxFd));
  }
  const pid_t writer = fork();
  if (writer == 0) {
    close(fixture.pipeEnds[0]);
    close(fixture.file);
    close(fixture.terminal);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGALRM, SIG_DFL);
    alarm(20);
    const int fd = fixture.pipeEnds[1];
    bool ok = Send(fd, kPrefix, sizeof(kPrefix) - 1) && Send(fd, kSuffix, sizeof(kSuffix) - 1);
    if (flood) {
      char block[8192];
      std::memset(block, 'x', sizeof(block));
      block[sizeof(block) - 1] = '\n';
      // More than 8 MiB, followed by a distinct unterminated tail. This is a
      // fixed byte count, not a timing/progress loop. All post-cap bytes must drain.
      for (int i = 0; ok && i < 1040; ++i) {
        ok = Send(fd, block, sizeof(block));
      }
    }
    ok = ok && Send(fd, kTail, sizeof(kTail) - 1);
    close(fd); // EOF only after reaching the tail, including the flood case
    _exit(ok ? 0 : 3);
  }
  fixture.ClosePipe(); // neither reader nor writer is kept alive by the parent
  if (!Check(writer >= 0, "fork flood writer")) {
    Reap(copier, "copier exits after failed writer fork and EOF");
    return;
  }
  Reap(writer, "writer reaches tail without SIGPIPE, failed writes or timeout");
  Reap(copier, "production copier exits normally after EOF");
  int status = 0;
  errno = 0;
  Check(waitpid(copier, &status, WNOHANG) == -1 && errno == ECHILD, "copier was reaped, not left running/zombie");

  struct stat info {};
  if (!Check(fstat(fixture.file, &info) == 0, "stat finished log")) {
    return;
  }
  if (!Check(info.st_size >= 0 && info.st_size <= static_cast< off_t >(kExpectedCap),
             "log including header and marker is at most 8 MiB")) {
    return;
  }
  std::string output(static_cast< size_t >(info.st_size), '\0');
  size_t used = 0;
  while (used < output.size()) {
    const ssize_t n = pread(fixture.file, output.data() + used, output.size() - used, static_cast< off_t >(used));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (!Check(n > 0, "read completed log")) {
      return;
    }
    used += static_cast< size_t >(n);
  }
  const std::string beginning = std::string(kHeader) + "split ~/disc.iso\n";
  Check(output.starts_with(beginning), "header retained and split path redacted in actual copier");
  Check(output.find(kPrivatePath) == std::string::npos, "no private path stored");
  const size_t marker = output.find(kExpectedMarker);
  if (flood) {
    Check(output.size() == kExpectedCap, "flood uses exact budget including initial header and reserved marker");
    Check(marker != std::string::npos && marker == output.rfind(kExpectedMarker) &&
              output.ends_with(kExpectedMarker), "exactly one explicit truncation marker terminates capped file");
    Check(output.find("WRITER_REACHED_TAIL") == std::string::npos, "post-cap tail discarded only from file");
  } else {
    Check(marker == std::string::npos, "short log is not marked truncated");
    Check(output == beginning + "WRITER_REACHED_TAIL ~/final", "EOF flush preserves redacted unterminated tail");
  }
}
} // namespace

int main() {
  CheckSplitPrivacy();
  RunCopy(false);
  RunCopy(true);
  if (sFailures != 0) {
    std::fprintf(stderr, "port_h700_log_copy_tests: %d failure(s)\n", sFailures);
    return 1;
  }
  std::puts("port_h700_log_copy_tests: all passed");
  return 0;
}
