#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <resolv.h>

#define USE_NETLINK (0)

// Replace these with your built-in host and port values
#ifndef DEFAULT_HOST
#define DEFAULT_HOST "example.com"
#endif // DEFAULT_HOST

#ifndef DEFAULT_PORT
#define DEFAULT_PORT 8910
#endif // DEFAULT_PORT

// Add your built-in certificate here
static char const* kCertString =
  "-----BEGIN CERTIFICATE-----\n"
  ". . .\n"
  "-----END CERTIFICATE-----\n";


// netlink
#if defined(__linux__) && USE_NETLINK
  #include <linux/netlink.h>
  #include <linux/rtnetlink.h>
#endif

// for openpty():
#if defined(__linux__)
  #include <pty.h>
#elif defined(__APPLE__)
  #include <util.h>
#endif


#include <openssl/ssl.h>
#if (SSLEAY_VERSION_NUMBER >= 0x0907000L)
# include <openssl/conf.h>
#endif

#include <openssl/err.h>





enum Params {
  kActivityTimeout_ms =       (5 * 60 * 1000),    // 5 minutes
};


////////////////////////////////////////////////////////////////

uint64_t  SystemTimePoint(void)
{
  struct timespec timePoint;
  clock_gettime(CLOCK_REALTIME, &timePoint);
  return(((uint64_t)timePoint.tv_sec * 1000) + ((uint64_t)timePoint.tv_nsec / 1000000));
}

uint64_t  SystemTimeStepMs(uint64_t earlierPoint, uint64_t laterPoint)
{
  return(laterPoint - earlierPoint);
}


////////////////////////////////////////////////////////////////

typedef struct Process {
  char const* const* args;
  char const* const* env;
  char const* cwd;
  int uid;

  int pid;
  int childStdin;
  int childStdout;
  int childStderr;
  int isActive;
  int exitStatus;
} Process;

void  ProcessDestroy(struct Process* process);  // forward declaration for use in ProcessCreate

Process* ProcessCreate(char const* const* args, char const* const* env, char const* cwd) {
  Process* process = (Process*)malloc(sizeof(Process));
  if(!process) {
    return(0);
  }
  memset(process, 0, sizeof(Process));
  process->pid = -1;
  process->childStdin = -1;
  process->childStdout = -1;
  process->childStderr = -1;

  if(args) {
    int numArgs = 0;
    for(char const* const* s = args; *s; s++) {
      numArgs++;
    }
    char** processArgs = malloc(sizeof(char const*) * (numArgs + 1));
    if(!processArgs) {
      ProcessDestroy(process);
      return(0);
    }
    for(int i = 0; i < numArgs; i++) {
      processArgs[i] = strdup(args[i]);
    }
    processArgs[numArgs] = 0;
    process->args = (char const* const*)processArgs;
  }

  if(env) {
    int numEnvs = 0;
    for(char const* const* e = env; *e; e++) {
      numEnvs++;
    }
    char** processEnv = malloc(sizeof(char const*) * (numEnvs + 1));
    if(!processEnv) {
      ProcessDestroy(process);
      return(0);
    }
    for(int i = 0; i < numEnvs; i++) {
      processEnv[i] = strdup(env[i]);
    }
    processEnv[numEnvs] = 0;
    process->env = (char const* const*)processEnv;
  }

  if(cwd) {
    process->cwd = strdup(cwd);
  }

  return(process);
}

void  ProcessDestroy(struct Process* process) {
  if(process->args) {
    for(char const* const* s = process->args; *s; s++) {
      free((char*)*s);
    }
    free((char**)process->args);
  }
  if(process->env) {
    for(char const* const* e = process->env; *e; e++) {
      free((char*)*e);
    }
    free((char**)process->env);
  }
  if(process->cwd) {
    free((char*)process->cwd);
  }
  // Note that the fds can all be the same in the PTY case.
  if(process->childStdin >= 0) {
    close(process->childStdin);
  }
  if((process->childStdout >= 0) && (process->childStdout != process->childStdin)) {
    close(process->childStdout);
  }
  if((process->childStderr >= 0) &&
     (process->childStderr != process->childStdin) &&
     (process->childStderr != process->childStdout)) {
    close(process->childStderr);
  }
  free(process);
}

int   ProcessStart(struct Process* process) {

  int childStdin[2], childStdout[2], childStderr[2];
  if((pipe(childStdin) != 0) || (pipe(childStdout) != 0) || (pipe(childStderr) != 0)) {
    fprintf(stderr, "Error: could not create pipes, error %i.\n", errno);
    process->isActive = 0;
    return(0);
  }

  pid_t pid = fork();

  if(pid == 0) {
    dup2(childStdin[0], STDIN_FILENO);
    dup2(childStdout[1], STDOUT_FILENO);
    dup2(childStderr[1], STDERR_FILENO);
    close(childStdin[0]);
    close(childStdin[1]);
    close(childStdout[0]);
    close(childStdout[1]);
    close(childStderr[0]);
    close(childStderr[1]);

    if(process->cwd) {
      chdir(process->cwd);
    }

    if(process->uid) {
      // Never run the child with elevated privileges if the drop failed.
      if(setuid(process->uid) != 0) {
        fprintf(stderr, "Error: could not drop to uid %i, error %i.\n", process->uid, errno);
        _exit(1);
      }
    }

    if(process->env) {
      for(char const* const* e = process->env; *e; e += 2) {
        setenv(e[0], e[1], 1);
      }
    }

    setsid();
    setpgid(0, 0);

    execv(process->args[0], (char**)&process->args[0]);

    fprintf(stderr, "Error: could not launch child process, error %i.\n", errno);

    _exit(1);

  } else if(pid < 0) {
    fprintf(stderr, "Error: could not fork, error %i.\n", errno);
    close(childStdin[0]);  close(childStdin[1]);
    close(childStdout[0]); close(childStdout[1]);
    close(childStderr[0]); close(childStderr[1]);
    process->isActive = 0;
    return(0);

  } else {
    close(childStdin[0]);
    fcntl(childStdout[0], F_SETFL, O_NONBLOCK);
    close(childStdout[1]);
    fcntl(childStderr[0], F_SETFL, O_NONBLOCK);
    close(childStderr[1]);

    process->pid = pid;
    process->childStdin = childStdin[1];
    process->childStdout = childStdout[0];
    process->childStderr = childStderr[0];
    process->isActive = 1;
    process->exitStatus = 0;

    //printf("pid=%i stdin=%i stdout=%i stderr=%i\n", process->pid, process->childStdin, process->childStdout, process->childStderr);

    return(1);
  }
}

int   ProcessStartPTY(struct Process* process) {

  int master = 0, slave = 0;
  struct winsize windowParams = {
    .ws_col = 80, .ws_row = 24,
    .ws_xpixel = 480, .ws_ypixel = 192
  };

  int ptySuccess = openpty(&master, &slave, 0, 0, &windowParams);
  if(ptySuccess != 0) {
    fprintf(stderr, "Error: could not open a tty, error %i.\n", errno);
    process->isActive = 0;
    return(0);
  }

  pid_t pid = fork();

  if(pid == 0) {

    if(process->cwd) {
      chdir(process->cwd);
    }

    if(process->uid) {
      // Never run the child with elevated privileges if the drop failed.
      if(setuid(process->uid) != 0) {
        fprintf(stderr, "Error: could not drop to uid %i, error %i.\n", process->uid, errno);
        _exit(1);
      }
    }

    if(process->env) {
      for(char const* const* e = process->env; *e; e += 2) {
        setenv(e[0], e[1], 1);
      }
    }

    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
    close(master);

    setsid();
    setpgid(0, 0);

    execv(process->args[0], (char**)&process->args[0]);

    fprintf(stderr, "Error: could not launch child process, error %i.\n", errno);

    _exit(1);

  } else if(pid > 0) {
    fcntl(master, F_SETFL, O_NONBLOCK);
    close(slave);

    process->pid = pid;
    process->childStdin = master;
    process->childStdout = master;
    process->childStderr = master;
    process->isActive = 1;
    process->exitStatus = 0;

    return(1);
  }
  else {
    fprintf(stderr, "Error: could not launch process, error %i.\n", errno);
    process->isActive = 0;
    return(0);
  }
}


int   ProcessStdin(struct Process* process) {
  return(process->childStdin);
}
int   ProcessStdout(struct Process* process) {
  return(process->childStdout);
}
int   ProcessStderr(struct Process* process) {
  return(process->childStderr);
}
int   ProcessPID(struct Process* process) {
  return(process->pid);
}

// Reap every child that has exited, since you shouldn't call waitpid() in multiple
//   places since that could cause a race condition.
void  ProcessReapChildren(struct Process* tracked) {
  int status = 0;
  pid_t pid;
  while((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    if(tracked && tracked->isActive && (tracked->pid == pid) &&
       (WIFEXITED(status) || WIFSIGNALED(status))) {
      tracked->exitStatus = WIFEXITED(status)? WEXITSTATUS(status) : -1;
      tracked->isActive = 0;
    }
  }
}

int   ProcessIsRunning(struct Process* process) {
  return(process->isActive);
}

int   ProcessGetExitCode(struct Process* process) {
  return(process->isActive? 0 : process->exitStatus);
}

int   ProcessSendUNIXSignal(struct Process* process, int signalNum) {
  kill(process->pid, signalNum);  // process
  kill(-process->pid, signalNum); // process group
  return(1);
}

int   ProcessResizeWindow(struct Process* process, int cols, int rows) {
  struct winsize newWindowParams = {
    .ws_col = cols, .ws_row = rows,
    .ws_xpixel = (cols * 6), .ws_ypixel = (rows * 8)
  };
  ioctl(process->childStdout, TIOCSWINSZ, &newWindowParams);
  return(0);
}

int   ProcessTerminate(struct Process* process) {
  return(ProcessSendUNIXSignal(process, SIGTERM));
}

////////////////////////////////////////////////////////////////

struct MuxChannel;

typedef enum MuxControlParseState {
  kMuxControlParseStateCommand,
  kMuxControlParseStateCloseChannel,
} MuxControlParseState;

typedef struct Mux {
  struct MuxChannel*    channels;

  MuxControlParseState  parseState;

  size_t                nextSegment;
  struct MuxChannel*    currChannel;
  int                   currChannelID;

  unsigned char         outputBuffer[258];
  size_t                outputBufferOffset;
  size_t                outputBufferRemaining;

} Mux;

typedef enum MuxChannelState {
  kChannelStatePending,
  kChannelStateNew,
  kChannelStateActive,
  kChannelStateClosing,
} MuxChannelState;


typedef struct MuxChannel {
  struct Mux*         mux;
  struct MuxChannel*  next;
  int                 id;
  MuxChannelState     state;
  int                 type;
  int                 muxInFD;
  int                 muxOutFD;

} MuxChannel;


struct Mux*         MuxCreate(void) {
  Mux* mux = (Mux*)malloc(sizeof(Mux));
  memset(mux, 0, sizeof(Mux));
  mux->currChannelID = -1;

  return(mux);
}

void                MuxDestroy(struct Mux* mux) {
  for(MuxChannel* c = mux->channels; c;) {
    MuxChannel* cn = c->next;
    free(c);
    c = cn;
  }
  free(mux);
}

enum {
  kControlMessageCloseChannel =           'x',
  kControlMessageCloseChannelAck =        'X',
  kControlMessageOpenConsole =            'c',
  kControlMessageOpenConsoleAck =         'C',
  kControlMessageOpenPath =               'p',
  kControlMessageOpenPathAck =            'P',
  kControlMessageOpenPathCompressed =     'z',
  kControlMessageOpenPathCompressedAck =  'Z',
};

size_t              muxControlRead(struct Mux* mux, void* bytes, size_t length) {
  size_t written = 0;
  while(length > 0) {
    int empty = 1;
    // report channel-closed events (from both solicited and unsolicited causes)
    for(MuxChannel** c = &mux->channels; *c;) {
      if((*c)->state == kChannelStateClosing) {
        if(length >= 2) {
          ((unsigned char*)bytes)[0] = kControlMessageCloseChannel;
          ((unsigned char*)bytes)[1] = (unsigned char)(*c)->id;
          bytes = (void*)(((unsigned char const*)bytes) + 2);
          length -= 2;
          written += 2;
          empty = 0;

          // remove from list
          MuxChannel* channel = (*c);
          *c = (*c)->next;
          free(channel);
          continue;
        }
      }
      c = &(*c)->next;
    }

    // report channel-opened events (solicited cause only)
    for(MuxChannel* channel = mux->channels; channel; channel = channel->next) {
      if(channel->state == kChannelStateNew) {
        if(length >= 2) {
          // assign ID and mark as active
          int nextChannelID = 1;
          for(MuxChannel const* cScan = mux->channels; cScan;) {
            if(cScan->id == nextChannelID) {
              nextChannelID = cScan->id + 1;
              cScan = mux->channels;
            }
            else {
              cScan = cScan->next;
            }
          }
          channel->id = nextChannelID;

          channel->state = kChannelStateActive;

          // prepare a response
          int response = -1;
          switch(channel->type) {
          case kControlMessageCloseChannel:
            response = kControlMessageCloseChannelAck;
            break;
          case kControlMessageOpenConsole:
            response = kControlMessageOpenConsoleAck;
            break;
          case kControlMessageOpenPath:
            response = kControlMessageOpenPathAck;
            break;
          case kControlMessageOpenPathCompressed:
            response = kControlMessageOpenPathCompressedAck;
            break;
          }
          ((unsigned char*)bytes)[0] = (unsigned char)response;
          ((unsigned char*)bytes)[1] = (unsigned char)channel->id;
          bytes = (void*)(((unsigned char const*)bytes) + 2);
          length -= 2;
          written += 2;
          empty = 0;
        }
      }
    }
    if(empty) {
      break;
    }
  }
  return(written);
}

MuxChannel*         muxCreatePendingChannel(struct Mux* mux, int type) {
  MuxChannel* channel = (MuxChannel*)malloc(sizeof(MuxChannel));
  memset(channel, 0, sizeof(MuxChannel));

  channel->mux = mux;
  channel->id = -1;
  channel->muxInFD = -1;
  channel->muxOutFD = -1;
  channel->state = kChannelStatePending;
  channel->type = type;

  MuxChannel** m = &mux->channels;
  while(*m && (*m)->next) {
    m = &((*m)->next);
  }
  *m = channel;
  return(channel);
}

size_t              muxControlChannel(struct Mux* mux, void const* bytes, size_t length) {
  size_t consumed = 0;
  while(length > 0) {
    unsigned char b = ((unsigned char const*)bytes)[0];
    switch(mux->parseState) {
    case kMuxControlParseStateCommand:
      switch(b) {
      case kControlMessageCloseChannel:
        mux->parseState = kMuxControlParseStateCloseChannel;
        break;
      case kControlMessageOpenConsole:
      case kControlMessageOpenPath:
      case kControlMessageOpenPathCompressed:
        {
          int type = (int)b;
          muxCreatePendingChannel(mux, type);
        }
        break;
      }
      break;

    case kMuxControlParseStateCloseChannel:
      {
        int channelID = (int)b;
        MuxChannel* c;
        for(c = mux->channels; c; c = c->next) {
          if(c->id == channelID) {
            c->state = kChannelStateClosing;
            break;
          }
        }
        //@@todo: if(!c) { nack }
        mux->parseState = kMuxControlParseStateCommand;
      }
    }
    bytes = (void const*)((unsigned char const*)bytes) + 1;
    length--;
    consumed++;
  }
  return(consumed);
}

int                 MuxBindPendingChannel(struct MuxChannel* channel, int muxInFD, int muxOutFD) {
  MuxChannel** m = &channel->mux->channels;
  while((*m != channel) && (*m)->next) {
    m = &((*m)->next);
  }
  if((*m != channel) || (channel->state != kChannelStatePending)) {
    return(0);
  }

  channel->muxInFD = muxInFD;
  channel->muxOutFD = muxOutFD;
  channel->state = kChannelStateNew;

  return(1);
}

int                 MuxCloseChannel(struct MuxChannel* channel) {
  MuxChannel** m = &channel->mux->channels;
  while((*m != channel) && (*m)->next) {
    m = &((*m)->next);
  }
  if(*m != channel) {
    return(0);
  }

  channel->state = kChannelStateClosing;

  return(1);
}

struct MuxChannel*  MuxGetPendingNewChannel(struct Mux* mux) {
  for(MuxChannel* c = mux->channels; c; c = c->next) {
    if(c->state == kChannelStatePending) {
      return(c);
    }
  }
  return(0);
}

struct MuxChannel const*  MuxGetFirstActiveChannel(struct Mux* mux) {
  for(MuxChannel* c = mux->channels; c; c = c->next) {
    if(c->state == kChannelStateActive) {
      return(c);
    }
  }
  return(0);
}

struct MuxChannel const*  MuxGetNextActiveChannel(struct MuxChannel const* channel) {
  for(MuxChannel* c = channel->next; c; c = c->next) {
    if(c->state == kChannelStateActive) {
      return(c);
    }
  }
  return(0);
}

int                 MuxChannelIsActive(struct MuxChannel const* channel) {
  return(channel->state == kChannelStateActive);
}

int                 MuxChannelGetMuxOutFD(struct MuxChannel const* channel) {
  return(channel->muxOutFD);
}

int                 MuxChannelGetMuxInFD(struct MuxChannel const* channel) {
  return(channel->muxInFD);
}

size_t              MuxWriteBytes(struct Mux* mux, void const* bytes, size_t length) {
  size_t consumed = 0;
  while(length > 0) {
    if(mux->nextSegment == 0) {
      mux->nextSegment = (1 + ((char const*)bytes)[0]);
      mux->currChannel = 0;
      mux->currChannelID = -1;
      bytes = (void*)(((char const*)bytes) + 1);
      length--;
      consumed++;
    }
    else if(mux->currChannelID == -1) {
      int selectedChannel = ((char const*)bytes)[0];

      if(selectedChannel == 0) {
        mux->currChannel = 0;
        mux->currChannelID = 0;
      }
      else {
        for(MuxChannel* c = mux->channels; c; c = c->next) {
          if(c->id == selectedChannel) {
            mux->currChannel = c;
            mux->currChannelID = selectedChannel;
            break;
          }
        }
      }

      bytes = (void*)(((char const*)bytes) + 1);
      length--;
      consumed++;
    }
    else {
      size_t chunk = (mux->nextSegment < length)? mux->nextSegment : length;

      if(mux->currChannel != 0) {
        size_t written = 0;
        for(written = 0; written < chunk;) {
          ssize_t w = write(mux->currChannel->muxOutFD, &((char const*)bytes)[written], chunk - written);
          if(w <= 0) {
            break;
          }
          written += w;
        }
        consumed += written;
      }
      else if(mux->currChannelID == 0) {
        consumed += muxControlChannel(mux, bytes, chunk);
      }
      else {
        consumed += chunk; // drain the bytes into nothing (error?)
      }

      bytes = (void*)(((char const*)bytes) + chunk);
      length -= chunk;
      mux->nextSegment -= chunk;

      if(mux->nextSegment == 0) {
        mux->currChannel = 0;
        mux->currChannelID = -1;
      }
    }
  }
  return(consumed);
}

size_t              MuxReadBytesForReadyFD(struct Mux* mux, void* bytes, size_t length, int readyFD) {
  size_t written = 0;
  while(length > 0) {
    if(mux->outputBufferRemaining > 0) {
      size_t chunk = (mux->outputBufferRemaining < length)? mux->outputBufferRemaining : length;
      memcpy(bytes, &mux->outputBuffer[mux->outputBufferOffset], chunk);
      mux->outputBufferOffset += chunk;
      mux->outputBufferRemaining -= chunk;
      bytes = (void*)((char*)bytes) + chunk;
      length -= chunk;
      written += chunk;
    }
    else {
      int empty = 1;
      size_t bytesRead = muxControlRead(mux, &mux->outputBuffer[2], sizeof(mux->outputBuffer) - 2);
      if(bytesRead > 0) {
        mux->outputBuffer[0] = ((unsigned char)bytesRead) - 1;
        mux->outputBuffer[1] = 0;
        mux->outputBufferOffset = 0;
        mux->outputBufferRemaining = bytesRead + 2;
        empty = 0;
      }
      else if(readyFD >= 0) {
        for(MuxChannel const* c = mux->channels; c; c = c->next) {
          if(c->muxInFD == readyFD) {
            ssize_t bytesRead = read(c->muxInFD, &mux->outputBuffer[2], sizeof(mux->outputBuffer) - 2);
            if(bytesRead > 0) {
              mux->outputBuffer[0] = ((unsigned char)bytesRead) - 1;
              mux->outputBuffer[1] = (unsigned char)c->id;
              mux->outputBufferOffset = 0;
              mux->outputBufferRemaining = bytesRead + 2;
              empty = 0;
            }
            break;
          }
        }
      }
      if(empty) {
        break;
      }
    }
  }
  return(written);
}

size_t              MuxReadBytes(struct Mux* mux, void* bytes, size_t length) {
  return(MuxReadBytesForReadyFD(mux, bytes, length, -1));
}

int                 MuxNeedsRead(struct Mux* mux) {
  // if any channels are pending setup or tear-down then we need to report that
  for(MuxChannel* c = mux->channels; c; c = c->next) {
    if((c->state == kChannelStateNew) || (c->state == kChannelStateClosing)) {
      return(1);
    }
  }
  return(0);
}

int unitTestMux(int argc, char** argv) {
  int errors = 0;
  char const* ipipe = 0;
  char const* opipe = 0;

  while(!errors) {
    static struct option commandLineOptions[] = {
      {"help",    no_argument,        0, '?'},

      {"ipipe",   required_argument,  0, 'i'},
      {"opipe",   required_argument,  0, 'o'},

      {0, 0, 0, 0}
    };
  
    int optionIndex = 0;
    int c = getopt_long(argc, argv, "i:o:md", commandLineOptions, &optionIndex);

    if(c == -1) {
      break;
    }

    switch(c) {
    case 'i': ipipe = optarg; break;
    case 'o': opipe = optarg; break;

    default:
      fprintf(stderr, "Fatal error: unrecognized argument \"%s\".\n", argv[optind - 1]);
      // (intentionally no break)
    case '?':
      fprintf(  stderr,
                "Unit-test Mux usage:\n"
                "  -m, --mux: mux\n"
                "  -d, --demux: demux\n"
                "  -i, --ipipe <path>: specify input pipe\n"
                "  -o, --opipe <path>: specify output pipe\n"
              );
      errors = 1;
      break;
    }
  }

  if(!ipipe || !opipe) {
    fprintf(stderr, "Error: Missing --ipipe or --opipe option (both are required)\n");
    return(-1);
  }

  struct Mux* m = MuxCreate();

  fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);  // set non-blocking
  struct termios s;
  tcgetattr(STDIN_FILENO, &s);
  s.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  s.c_oflag &= ~OPOST;
  s.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  s.c_cflag &= ~(CSIZE | PARENB);
  s.c_cflag |= CS8;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &s);

  int muxIn = open(ipipe, O_RDONLY | O_NONBLOCK);
  int muxOut = open(opipe, O_WRONLY);

  if(muxOut < 0) {
    fprintf(stderr, "Error: Cannot open output pipe \"%s\".\n", opipe);
    return(-1);
  }
  if(muxIn < 0) {
    fprintf(stderr, "Error: Cannot open input pipe \"%s\".\n", ipipe);
    return(-1);
  }

  // special test hack:
  write(muxOut, "\x00\x00""c", 3);  // packet for channel 0 (control) instruction "create console"

  fd_set readSet, writeSet;

  char buffer[1000];
  while(1) {
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    struct MuxChannel* c;
    while((c = MuxGetPendingNewChannel(m)) != 0) {
      fprintf(stderr, "Binding stdin/stdout\n");
      MuxBindPendingChannel(c, STDIN_FILENO, STDOUT_FILENO);
    }
    int topFD = -1;
    for(struct MuxChannel const* c = MuxGetFirstActiveChannel(m); c; c = MuxGetNextActiveChannel(c)) {
      FD_SET(MuxChannelGetMuxInFD(c), &readSet);
      if(MuxChannelGetMuxInFD(c) > topFD) {
        topFD = MuxChannelGetMuxInFD(c);
      }
    }
    FD_SET(muxIn, &readSet);
    if(muxIn > topFD) {
      topFD = muxIn;
    }
    if(MuxNeedsRead(m)) {
      FD_SET(muxOut, &writeSet);
      if(muxOut > topFD) {
        topFD = muxOut;
      }
    }

    int result = select(topFD + 1, &readSet, &writeSet, 0, 0);

    if(FD_ISSET(muxOut, &writeSet)) {
      size_t muxBytesRead = MuxReadBytes(m, buffer, sizeof(buffer));
      fprintf(stderr, "<<#%i(%zi) ", muxOut, muxBytesRead);
      for(int i = 0; i < muxBytesRead; i++) {
        fprintf(stderr, "%02x ", buffer[i]);
      }
      fprintf(stderr, "<<\n");

      for(size_t written = 0; written < muxBytesRead;) {
        ssize_t w = write(muxOut, &buffer[written], muxBytesRead - written);
        if(w <= 0) {
          break;
        }
        written += w;
      }
    }

    for(struct MuxChannel const* c = MuxGetFirstActiveChannel(m); c; c = MuxGetNextActiveChannel(c)) {
      if(FD_ISSET(MuxChannelGetMuxInFD(c), &readSet)) {
        fprintf(stderr, "!%i\n", MuxChannelGetMuxInFD(c));
        size_t muxBytesRead = MuxReadBytesForReadyFD(m, buffer, sizeof(buffer), MuxChannelGetMuxInFD(c));

        fprintf(stderr, "<<#%i(%zi) ", muxOut, muxBytesRead);
        for(int i = 0; i < muxBytesRead; i++) {
          fprintf(stderr, "%02x ", buffer[i]);
        }
        fprintf(stderr, "<<\n");

        for(size_t written = 0; written < muxBytesRead;) {
          ssize_t w = write(muxOut, &buffer[written], muxBytesRead - written);
          if(w <= 0) {
            break;
          }
          written += w;
        }
      }
    }

    if(FD_ISSET(muxIn, &readSet)) {
      fprintf(stderr, "!%i\n", muxIn);
      ssize_t muxInBytesRead = read(muxIn, buffer, sizeof(buffer));

      if(muxInBytesRead > 0) {
        fprintf(stderr, ">>#%i(%zi) ", muxIn, muxInBytesRead);
        for(int i = 0; i < muxInBytesRead; i++) {
          fprintf(stderr, "%02x ", buffer[i]);
        }
        fprintf(stderr, ">>\n");

        size_t muxBytesWritten = MuxWriteBytes(m, buffer, muxInBytesRead);

        fprintf(stderr, "#mux(%zi)\n", muxBytesWritten);
        // @@check result
      }
    }
  }
  fcntl(STDIN_FILENO, F_SETFL, 0);  // clear non-blocking

  return(0);
}


////////////////////////////////////////////////////////////////


typedef struct DNSResult {
  struct DNSResult* next;
  struct sockaddr_in  address;

} DNSResult;

DNSResult const*  DNSQuery(char const* fqdn) {
  struct __res_state resolverState = {0};

  if(res_ninit(&resolverState) != 0) {
    fprintf(stderr, "Error: could not initialize the resolver, error %i.\n", errno);
    return(0);
  }

  unsigned char dnsAnswer[256] = {0};
  int dnsAnswerLength = res_nquery(   &resolverState,
                                      fqdn,
                                      1, 1, // INTERNET and A record
                                      dnsAnswer, 256
                                    );
  if(dnsAnswerLength < 0) {
    res_nclose(&resolverState);
    return(0);
  }

  ns_msg dnsMessage = {0};
  if(ns_initparse(dnsAnswer, dnsAnswerLength, &dnsMessage) < 0) {
    res_nclose(&resolverState);
    return(0);
  }

  DNSResult* firstResult = 0;
  DNSResult** lastResult = &firstResult;

  for(int i = 0; i < ns_msg_count(dnsMessage, ns_s_an); i++) {
    ns_rr record;
    ns_parserr(&dnsMessage, ns_s_an, i, &record);

    #if 0 // debug
    fprintf(stderr, "#info: DNS[%i] = type: %i class: %i ttl: %i data[%i] = { ", i, ns_rr_type(record), ns_rr_class(record), ns_rr_ttl(record), ns_rr_rdlen(record));
    for(int j = 0; j < ns_rr_rdlen(record); j++) {
      int b = ns_rr_rdata(record)[j];
      fprintf(stderr, "%c%c ", "0123456789ABCDEF"[(b >> 4) & 0xF], "0123456789ABCDEF"[b & 0xF]);
    }
    fprintf(stderr, "}\n");
    #endif // end debug

    *lastResult = (DNSResult*)malloc(sizeof(DNSResult));
    if(!*lastResult) {
      break;  // out of memory: return whatever was resolved so far
    }
    memset(*lastResult, 0, sizeof(DNSResult));
    size_t size = sizeof((*lastResult)->address.sin_addr.s_addr);
    if(ns_rr_rdlen(record) < size)
      size = ns_rr_rdlen(record);
    memcpy(&(*lastResult)->address.sin_addr.s_addr, ns_rr_rdata(record), size);
    (*lastResult)->next = 0;
    lastResult = &((*lastResult)->next);
  }

  res_nclose(&resolverState);
  return(firstResult);
}

void              DNSResultFree(DNSResult const* result) {
  while(result) {
    DNSResult* toFree = (DNSResult*)result;
    result = result->next;
    free(toFree);
  }
}

////////////////////////////////////////////////////////////////


static volatile int gExitRequested = 0;
struct Process* gProcess = 0;
static volatile sig_atomic_t gChildSignaled = 0;

void    onTermSignal(int signal) {
  (void)signal;
  fprintf(stderr, "Received signal %i, exiting\n", signal);
  gExitRequested = 1;
}

void    onSigchldSignal(int signal, siginfo_t* info, void* user) {
  (void)signal;
  (void)info;
  (void)user;
  // Do NOT reap here. All reaping happens on the main thread via
  // ProcessReapChildren(), so there is exactly one waitpid() owner and no race
  // over the child's exit status (bug B5). This handler only records that a
  // child changed state; because it is installed without SA_RESTART, its
  // delivery also interrupts the loop's select() so the child is reaped and
  // restarted promptly rather than after the 1-second timeout.
  gChildSignaled = 1;
}

void  AsyncSet(int fd, fd_set* set, int* top) {
  if(fd > *top) {
    *top = fd;
  }
  FD_SET(fd, set);
}

int verifyCallback(int preverify, X509_STORE_CTX* x509Context) {
  int depth = X509_STORE_CTX_get_error_depth(x509Context);
  int err = X509_STORE_CTX_get_error(x509Context);

  X509* cert = X509_STORE_CTX_get_current_cert(x509Context);
  X509_NAME* iname = cert ? X509_get_issuer_name(cert) : NULL;
  X509_NAME* sname = cert ? X509_get_subject_name(cert) : NULL;

  if(depth == 0) {
    // If depth is 0, its the server's certificate.
  }
  if(!preverify) {
    fprintf(stderr, "Error: Certificate mismatch.\n");
  }

  return(preverify);
}


// Push as much buffered data as we can here, the rest will write later.
int TLSWriteBuffered(SSL* tls, unsigned char* buffer, size_t* offset) {
  if((*offset == 0) || !tls || !SSL_is_init_finished(tls)) {
    return(0);
  }
  int written = SSL_write(tls, buffer, (int)*offset);
  if(written > 0) {
    if((size_t)written < *offset) {
      memmove(buffer, &buffer[written], *offset - (size_t)written);
    }
    *offset -= (size_t)written;
    return(1);
  }
  int sslError = SSL_get_error(tls, written);
  if((sslError == SSL_ERROR_WANT_WRITE) || (sslError == SSL_ERROR_WANT_READ)) {
    return(1);  // retriable: data retained, flush ciphertext and try again later
  }
  // Handle a fatal write error by dropping bytes, since there's nothing else we can do.
  fprintf(stderr, "Error: OpenSSL write error (%i); dropping %zu buffered byte(s).\n", sslError, *offset);
  *offset = 0;
  return(0);
}


void showUsage(void) {
  fprintf(  stderr,
            "Usage:\n"
            "\n"
            "Required:\n"
            "  -r, --remote <hostname>: Fully-qualified server domain name to connect to\n"
            "  -p, --port <num>:        Server port to connect to\n"
            "  -c, --cert <path>:       Path to server certificate\n"
            "\n"
            "Optional:\n"
            "  -s, --shell <path>:      Path of \"shell\" command to invoke, default /bin/bash\n"
            "  -d, --cwd <path>:        Working directory of the shell or command, default $CWD\n"
            "  -i, --ident <path>:      Identity file, read and used for device identity\n"
            "\n"
            "Examples:\n"
            "\n"
            "  (Example TBD)\n"
            "    --remote server.example.com --port 8123 --cert /path/to/cert.pem\n"
            "\n"
          );
}


int main(int argc, char** argv) {

  int errors = 0;

  char const* remoteHost = 0;
  int remotePort = 0;
  char const* certPath = 0;
  char const* certString = 0;
  char const* shellPath = 0;
  char const* shellCWD = 0;
  char const* shellIDFile = 0;

  while(!errors)
  {
    static struct option commandLineOptions[] =
    {
      {"help",      no_argument,        0, '?'},

      {"remote",    required_argument,  0, 'r'},
      {"port",      required_argument,  0, 'p'},
      {"cert",      required_argument,  0, 'c'},
      {"shell",     required_argument,  0, 's'},
      {"cwd",       required_argument,  0, 'd'},
      {"ident",     required_argument,  0, 'i'},

      {"a",         no_argument,        0, 'a'},

      {"unit-test", required_argument,  0, 300},

      {0, 0, 0, 0}
    };
  
    int optionIndex = 0;
    int c = getopt_long(argc, argv, "r:p:c:s:d:i:a", commandLineOptions, &optionIndex);

    if(c == -1) {
      break;
    }

    switch(c) {
    case 'r': remoteHost = optarg;        break;
    case 'p': remotePort = atoi(optarg);  break;
    case 'c': certPath = optarg;          break;
    case 's': shellPath = optarg;         break;
    case 'd': shellCWD = optarg;          break;
    case 'i': shellIDFile = optarg;       break;

    case 'a':
      remoteHost = DEFAULT_HOST;
      remotePort = DEFAULT_PORT;
      certPath = 0;
      certString = kCertString;
      break;

    case 300:
      if(!strcmp(optarg, "mux")) {
        return(unitTestMux(argc, argv));
      }
      break;

    default:
      fprintf(stderr, "Fatal error: unrecognized argument \"%s\".\n", argv[optind - 1]);
      // (intentionally no break)
    case '?':
      errors++;
      break;
    }
  }

  if(!remoteHost) {
    fprintf(stderr, "Fatal error: no remote host specified.  Use --remote to specify.\n");
    errors++;
  }

  if(!remotePort) {
    fprintf(stderr, "Fatal error: no remote port number specified.  Use --port to specify.\n");
    errors++;
  }

  if(!certPath && !certString) {
    fprintf(stderr, "Fatal error: no certificate path specified.  Use --cert to specify.\n");
    errors++;
  }

  if(errors > 0) {
    showUsage();
    if(errors > 1) {
      fprintf(stderr, "Fatal error: Multiple errors, exiting.\n\n");
    }
    exit(1);
  }

  if(!shellPath) {
    shellPath = "/bin/bash";
  }

  // We need to be able to free `allocatedCWD` at exit
  char* allocatedCWD = 0;
  if(!shellCWD) {
    // Note that this might return 0 on failure, but that's ok, ProcessCreate() can accept 0.
    allocatedCWD = getcwd(0, 0);
    shellCWD = allocatedCWD;
  }

  
  // @@todo: re-read ident string in main loop to handle cases where serial number is missing, corrupted or changes
  char const* ident = 0;
  char identBuffer[256];
  char ps1Buffer[256];
  if(shellIDFile) {
    int shellIDFd = open(shellIDFile, O_RDONLY);
    if(shellIDFd >= 0)
    {
      char serial[256] = {0};
      ssize_t serialLen = read(shellIDFd, serial, sizeof(serial) - 1);
      if(serialLen > 0)
        serial[serialLen] = '\0';
      for(int i = 0; i < serialLen; i++) {
        if( !(  ((serial[i] >= 'A') && (serial[i] <= 'Z')) ||
                ((serial[i] >= 'a') && (serial[i] <= 'z')) ||
                ((serial[i] >= '0') && (serial[i] <= '9')) ||
                (serial[i] == '-') ||
                (serial[i] == '_')
              )
           )
        {
          serial[i] = '\0';
          break;
        }
      }
      close(shellIDFd);
      snprintf(identBuffer, sizeof(identBuffer), "%s", serial);
      ident = identBuffer;
    }
  }
  if(!ident) {
    snprintf(identBuffer, sizeof(identBuffer), "(no_system_id)");
    ident = identBuffer;
  }

  snprintf(ps1Buffer, sizeof(ps1Buffer), "%s:\\w \\$ ", ident);

  struct sigaction termSignal = {
    .sa_handler = &onTermSignal,
    .sa_flags = 0
  };
  
  sigemptyset(&termSignal.sa_mask);
  sigaction(SIGTERM, &termSignal, 0);
  sigaction(SIGINT, &termSignal, 0);

  struct sigaction sigChldSignal = {
    .sa_sigaction = &onSigchldSignal,
    .sa_flags = SA_SIGINFO
  };
  
  sigemptyset(&sigChldSignal.sa_mask);
  sigaction(SIGCHLD, &sigChldSignal, 0);

  // OpenSSL setup
  SSL_library_init();

  SSL_load_error_strings();

  OPENSSL_config(NULL);

  const SSL_METHOD* method = SSLv23_method();
  if(method == 0) {
    fprintf(stderr, "Error with SSLv23_method\n");
    exit(-1);
  }

  SSL_CTX* ctx = SSL_CTX_new(method);
  if(ctx == 0) {
    fprintf(stderr, "Error with SSL_CTX_new\n");
    exit(-1);
  }

  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, &verifyCallback);

  SSL_CTX_set_verify_depth(ctx, 4);

  const long flags = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION;
  SSL_CTX_set_options(ctx, flags);

  if(certPath && !certString) {
    long res = SSL_CTX_load_verify_locations(ctx, certPath, NULL);
    if(res != 1) {
      char const* errStr = ERR_error_string(ERR_get_error(), 0);
      fprintf(  stderr,
                "Fatal error: Unable to load or parse certificate at \"%s\": '%s'\n",
                certPath,
                errStr
              );
      exit(-1);
    }
  }
  else if(!certPath && certString) {
    BIO* certBio = BIO_new(BIO_s_mem());
    BIO_puts(certBio, certString);
    X509* cert = PEM_read_bio_X509(certBio, NULL, NULL, NULL);
    BIO_free(certBio);
    if(!cert) {
      char const* errStr = ERR_error_string(ERR_get_error(), 0);
      fprintf(  stderr,
                "Fatal error: Unable to load or parse built-in certificate: '%s'\n",
                errStr
              );
      exit(-1);
    }
    X509_STORE* certStore = SSL_CTX_get_cert_store(ctx);
    if(X509_STORE_add_cert(certStore, cert) != 1) {
      char const* errStr = ERR_error_string(ERR_get_error(), 0);
      fprintf(  stderr,
                "Fatal error: Unable to add a certificate to the certificate store: '%s'\n",
                errStr
              );
      exit(-1);
    }
    // This is needed to decrease the refcount, otherwise `cert` leaks.
    X509_free(cert);
  }

  BIO* tlsRead = 0;   // app writes data from the socket to this; OpenSSL lib reads from this
  BIO* tlsWrite = 0;  // app reads from this and sends to the socket; OpenSSL lib writes to this
  SSL* tls = 0;

  int tcpFD = -1, tlsMustWrite = 0, identified = 0;
  unsigned char processRx[256];
  size_t processRxOffset = 0;
  unsigned char txBuffer[256];
  size_t txBufferOffset = 0;

  uint64_t lastActivityTimepoint = SystemTimePoint();

  // Backoff capped at 60s if connections fail, resets to 0s upon success.
  uint64_t nextConnectAttempt = 0;
  unsigned int connectBackoffMs = 0;

#if defined(__linux__) && USE_NETLINK
  int netlinkSocket = -1;
#endif

  fd_set readSet, writeSet, errSet;

  while(!gExitRequested) {
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_ZERO(&errSet);

    int topFD = -1;

#if defined(__linux__) && USE_NETLINK
    while(netlinkSocket < 0) {
      struct sockaddr_nl netlinkAddr = {0};

      if((netlinkSocket = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE)) < 0) {
        fprintf(stderr, "Error: Unable to create netlink socket.\n");
        break;
      }

      netlinkAddr.nl_family = AF_NETLINK;
      netlinkAddr.nl_groups = RTMGRP_IPV4_ROUTE;

      if(bind(netlinkSocket, (struct sockaddr*)&netlinkAddr, sizeof(netlinkAddr)) < 0) {
        close(netlinkSocket);
        netlinkSocket = -1;
        fprintf(stderr, "Error: Unable to bind netlink address.\n");
        break;
      }

      fcntl(netlinkSocket, F_SETFL, O_NONBLOCK);

      break;
    }
#endif

    // disconnect if no activity has occurred for a certain amount of time
    uint64_t now = SystemTimePoint();
    if(SystemTimeStepMs(lastActivityTimepoint, now) > kActivityTimeout_ms) {
      lastActivityTimepoint = now;
      // @@todo: close function
      if(tcpFD >= 0) {
        close(tcpFD);
        tcpFD = -1;
      }
      if(tls) {
        SSL_free(tls);
        tls = 0;
      }
      tlsRead = 0;
      tlsWrite = 0;
      processRxOffset = 0;  // drop buffered output; it belongs to the closed connection
      if(gProcess) {
        ProcessTerminate(gProcess);
        ProcessDestroy(gProcess);
        gProcess = 0;
      }
    }


    while(tcpFD < 0) {

      if(now < nextConnectAttempt) {
        break;  // still within the reconnect backoff window
      }
      // Reserve the next attempt slot up front (this covers DNS failures too).
      connectBackoffMs = connectBackoffMs? ((connectBackoffMs < 30000)? (connectBackoffMs * 2) : 60000) : 1000;
      nextConnectAttempt = now + connectBackoffMs;

      DNSResult const* dnsResults = DNSQuery(remoteHost);
      DNSResult const* selectedHost = dnsResults;

      if(!dnsResults) {
        fprintf(stderr, "Error: No DNS results for \"%s\", will retry.\n", remoteHost);
        break;
      }

      // count results and pick a random one
      int resultCount = 0;
      while(1) {
        selectedHost = selectedHost->next;
        resultCount++;
        if(!selectedHost) {
          int selectedIndex = rand() % resultCount;
          selectedHost = dnsResults;
          for(int index = 0; index < selectedIndex; index++)
            selectedHost = selectedHost->next;
          break;
        }
      }

      tcpFD = socket(AF_INET, SOCK_STREAM, 0);
      if(tcpFD < 0) {
        fprintf(stderr, "Error: Unable to open socket\n");
        DNSResultFree(dnsResults);
        selectedHost = 0;
        break;
      }

      struct sockaddr_in addr;
      memcpy(&addr, &selectedHost->address, sizeof(struct sockaddr_in));
      addr.sin_family = AF_INET;
      addr.sin_port = htons(remotePort);
      DNSResultFree(dnsResults);
      selectedHost = 0;

    #if 0
      printf(   "Connecting to %s:%u (%u.%u.%u.%u)...\n",
                remoteHost,
                remotePort,
                (addr.sin_addr.s_addr >> 0) & 0xFF,
                (addr.sin_addr.s_addr >> 8) & 0xFF,
                (addr.sin_addr.s_addr >> 16) & 0xFF,
                (addr.sin_addr.s_addr >> 24) & 0xFF
              );
    #endif

      if(connect(tcpFD, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        // @@todo: decrease reporting rate of this message
        //fprintf(stderr, "Error: Connect error\n");
        close(tcpFD);
        tcpFD = -1;
        break;
      }
      // connected here

      fcntl(tcpFD, F_SETFL, O_NONBLOCK);

      tlsRead = BIO_new(BIO_s_mem());
      tlsWrite = BIO_new(BIO_s_mem());
      tls = SSL_new(ctx);

      if(!tlsRead || !tlsWrite || !tls) {
        fprintf(stderr, "Error: Error allocating TLS resources\n");
        if(tlsRead) {
          BIO_free(tlsRead);
          tlsRead = 0;
        }
        if(tlsWrite) {
          BIO_free(tlsWrite);
          tlsWrite = 0;
        }
        if(tls) {
          SSL_free(tls);
          tls = 0;
        }
        break;
      }

      SSL_set_connect_state(tls);
      SSL_set_bio(tls, tlsRead, tlsWrite);

      //printf("Reconnected to %s:%u\n", remoteHost, remotePort);
      identified = 0;

      lastActivityTimepoint = SystemTimePoint();

      SSL_do_handshake(tls);

      tlsMustWrite = 1;
      txBufferOffset = 0;

      // Connected: clear the reconnect backoff.
      connectBackoffMs = 0;
      nextConnectAttempt = 0;

      break;
    }

    if(!identified && tls && SSL_is_init_finished(tls)) {
      if(SSL_write(tls, ident, strlen(ident) + 1) != (strlen(ident) + 1)) {
        fprintf(stderr, "Error: OpenSSL write error, identification failed.\n");
      }
      else
        identified = 1;
    }

    if(!gProcess) {  // && TLS
      char const* const args[] = {
        shellPath,
        0
      };
      char const* const env[] = {
        "PS1", ps1Buffer,
        "TERM", "xterm",
        0
      };
      //fprintf(stderr, "Info: Process (re)started\n");
      gProcess = ProcessCreate(args, env, shellCWD);
      if(gProcess) {
        ProcessStartPTY(gProcess);
        ProcessResizeWindow(gProcess, 120, 50);
      } else {
        fprintf(stderr, "Error: could not allocate process; will retry.\n");
      }
    }

    // Retry processing the output that TLS couldn't accept on an earlier pass.
    if((processRxOffset > 0) && TLSWriteBuffered(tls, processRx, &processRxOffset)) {
      tlsMustWrite = 1;
    }

#if defined(__linux__) && USE_NETLINK
    if(netlinkSocket >= 0) {
      AsyncSet(netlinkSocket, &readSet, &topFD);
      AsyncSet(netlinkSocket, &errSet, &topFD);
    }
#endif

    if(tcpFD >= 0) {
      AsyncSet(tcpFD, &readSet, &topFD);
    }

    if((tcpFD >= 0) && (tlsMustWrite || (txBufferOffset > 0))) {
      AsyncSet(tcpFD, &writeSet, &topFD);
      tlsMustWrite = 0;
    }

    if(gProcess && (tcpFD >= 0) && tls && SSL_is_init_finished(tls)) {
      AsyncSet(ProcessStdout(gProcess), &readSet, &topFD);
      AsyncSet(ProcessStderr(gProcess), &readSet, &topFD);
    }

    struct timeval timeout = {
      .tv_sec = 1
    };

    int result = select(topFD + 1, &readSet, &writeSet, &errSet, &timeout);

    if(result > 0) {

#if defined(__linux__) && USE_NETLINK
      // Netlink info triggers reconnection, DNS refresh and so on
      if((netlinkSocket >= 0) && FD_ISSET(netlinkSocket, &readSet)) {
        char netlinkBuffer[4096];
        ssize_t netlinkRx;
        while((netlinkRx = recv(netlinkSocket, netlinkBuffer, sizeof(netlinkBuffer), 0)) > 0) {
          struct nlmsghdr* nlh = (struct nlmsghdr*)&netlinkBuffer;
          if(nlh->nlmsg_type == NLMSG_DONE)
            break;

          for(; NLMSG_OK(nlh, netlinkRx); nlh = NLMSG_NEXT(nlh, netlinkRx)) {
            printf("Info: Netlink item type %i\n", nlh->nlmsg_type);
            //struct rtmsg* route_entry = (struct rtmsg*)NLMSG_DATA(nlh);
          }
        }
      }
#endif

      // process stdout, stderr => SSL interface
      // process stdin <= SSL interface
      if(gProcess) {
        // Both stdout and stderr feed the same processRx buffer (in the PTY case
        // they are literally the same fd) and go out through TLSWriteBuffered,
        // which retains anything TLS can't accept yet so it isn't lost.
        if(FD_ISSET(ProcessStdout(gProcess), &readSet)) {
          ssize_t bytesRead = 0;
          while((processRxOffset < sizeof(processRx)) &&
                (bytesRead = read(ProcessStdout(gProcess), &processRx[processRxOffset], sizeof(processRx) - processRxOffset)) > 0) {
            processRxOffset += bytesRead;
            if(TLSWriteBuffered(tls, processRx, &processRxOffset)) {
              tlsMustWrite = 1;
            }
          }
        }
        if((ProcessStderr(gProcess) != ProcessStdout(gProcess)) &&
           FD_ISSET(ProcessStderr(gProcess), &readSet)) {
          ssize_t bytesRead = 0;
          while((processRxOffset < sizeof(processRx)) &&
                (bytesRead = read(ProcessStderr(gProcess), &processRx[processRxOffset], sizeof(processRx) - processRxOffset)) > 0) {
            processRxOffset += bytesRead;
            if(TLSWriteBuffered(tls, processRx, &processRxOffset)) {
              tlsMustWrite = 1;
            }
          }
        }
      }

      // TCP (encrypted) bytes => BIO
      // TCP (encrypted) bytes <= BIO
      if((tcpFD >= 0) && FD_ISSET(tcpFD, &readSet)) {
        // pass bytes to OpenSSL
        if(tlsRead) {
          tlsMustWrite = 1;

          char buffer[100];
          ssize_t bytesRead = 0, totalBytesRead = 0;
          while(1) {
            errno = 0;
            bytesRead = read(tcpFD, buffer, 100);
            if(!bytesRead || (!totalBytesRead && (bytesRead < 0) && (errno != ENOTCONN))) {
              fprintf(stderr, "Info: TCP %s (%s) %zi\n", (bytesRead < 0)? "error" : "closed", strerror(errno), bytesRead);
              // @@todo: close function
              close(tcpFD);
              tcpFD = -1;
              SSL_free(tls);
              tls = 0;
              tlsRead = 0;
              tlsWrite = 0;
              txBufferOffset = 0;
              processRxOffset = 0;
              ProcessTerminate(gProcess);
              ProcessDestroy(gProcess);
              gProcess = 0;
              break;
            } else if(bytesRead < 0) {
              break;
            }

            totalBytesRead += bytesRead;

            if(BIO_write(tlsRead, buffer, bytesRead) != bytesRead) {
              fprintf(stderr, "Error: OpenSSL I/O write error, TCP bytes lost.\n");
            }
            //printf("TCP => TLSio (%zu)\n", bytesRead);

            if(!SSL_is_init_finished(tls)) {
              int result = SSL_do_handshake(tls);
              if(SSL_is_init_finished(tls)) {
                X509* cert = SSL_get_peer_certificate(tls);
                if(cert) {
                  X509_free(cert);
                } else {
                  // Don't connect if the server doesn't present any certificate.
                  fprintf(stderr, "Error: server presented no certificate; dropping connection.\n");
                  close(tcpFD);
                  tcpFD = -1;
                  SSL_free(tls);
                  tls = 0;
                  tlsRead = 0;
                  tlsWrite = 0;
                  txBufferOffset = 0;
                  processRxOffset = 0;
                  if(gProcess) {
                    ProcessTerminate(gProcess);
                    ProcessDestroy(gProcess);
                    gProcess = 0;
                  }
                  break;  // leave the read loop; tls/tcpFD are now cleared
                }
              }
              int sslError = SSL_get_error(tls, result);
              switch(sslError) {
                case SSL_ERROR_WANT_WRITE:
                case SSL_ERROR_WANT_READ:
                  tlsMustWrite = 1;
                  break;
                default:
                  break;
              }
            } else {
              tlsMustWrite = 1;
            }
          }
        } else {
          fprintf(stderr, "TLS is not active, dropping received TCP bytes.\n");
        }
      }
      if((tcpFD >= 0) && FD_ISSET(tcpFD, &writeSet)) {
        ssize_t bytesRead = 0;
        while((txBufferOffset < sizeof(txBuffer)) &&
              (bytesRead = BIO_read(tlsWrite, &txBuffer[txBufferOffset], sizeof(txBuffer) - txBufferOffset)) > 0) {
          txBufferOffset += bytesRead;
          if(tls) {
            ssize_t written = write(tcpFD, txBuffer, txBufferOffset);
            if(written > 0) {
              memmove(txBuffer, &txBuffer[written], sizeof(txBuffer) - written);
              txBufferOffset -= written;
            } else {
              fprintf(stderr, "Error: TCP write error, OpenSSL bytes lost.\n");
            }
          } else {
            fprintf(stderr, "Error: TLS is not active, dropping TCP bytes to be sent.\n");
          }
        }
        tlsMustWrite = 0;
      }

      if(tls && SSL_is_init_finished(tls)) {
        char buffer[100];
        ssize_t bytesRead = 0;
        while((bytesRead = SSL_read(tls, buffer, 100)) > 0) {
          lastActivityTimepoint = SystemTimePoint();
          if(write(ProcessStdin(gProcess), buffer, bytesRead) != bytesRead) {
            fprintf(stderr, "Error: Process stdin write error, bytes lost\n");
          }
        }
        int sslError = SSL_get_error(tls, result);
        switch(sslError) {
          case SSL_ERROR_WANT_WRITE:
          case SSL_ERROR_WANT_READ:
            tlsMustWrite = 1;
            break;
          default:
            break;
        }
      }
    }

    // We should only reap here instead of in muliple places since it could be a race condition.
    if(gChildSignaled) {
      gChildSignaled = 0;
      ProcessReapChildren(gProcess);
    }

    if(gProcess && !ProcessIsRunning(gProcess)) {
      fprintf(  stderr,
                "Info: Shell process exited with code %u; restarting it.\n",
                ProcessGetExitCode(gProcess)
              );

      ProcessDestroy(gProcess);
      gProcess = 0;
      processRxOffset = 0;
    }
  }

  // Hang up TLS.
  if(tls) {
    SSL_free(tls);
    tls = 0;
    tlsWrite = 0;
    tlsRead = 0;
  }
  if(tcpFD >= 0) {
    close(tcpFD);
    tcpFD = -1;
  }

  fprintf(stderr, "Info: Shutting down...\n");
  if(gProcess) {
    while(ProcessIsRunning(gProcess)) {
      ProcessSendUNIXSignal(gProcess, SIGKILL);
      usleep(100000);
      ProcessReapChildren(gProcess);
    }
    ProcessDestroy(gProcess);
    gProcess = 0;
  }

  // This is a no-op if `allocatedCWD` is 0.
  free(allocatedCWD);

  return(0);
}
