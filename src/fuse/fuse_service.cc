#include "fuse_service.h"

#ifndef Q_OS_WIN

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/mount.h>
#include <signal.h>
#include <sys/param.h>
#include <sys/mount.h>

#include <ctime>

#include <QtConcurrent/QtConcurrent>
#include <QString>

#include "file_management/carrier_files_manager.h"
#include "encoders/hamming_encoder.h"
#include "encoders/encoder.h"
#include "permutations/permutation.h"
#include "permutations/affine_permutation.h"
#include "permutations/feistel_num_permutation.h"
#include "utils/stego_math.h"
#include "utils/config.h"

#ifdef __APPLE__
static const char  *file_path      = "/virtualdisc.dmg";
const char* FuseService::virtual_file_name_ = "virtualdisc.dmg";
#else
static const char  *file_path      = "/virtualdisc.iso";
const char* FuseService::virtual_file_name_ = "virtualdisc.iso";
#endif

static int sfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi);
static int sfs_getattr(const char *path, struct stat *stbuf);
static int sfs_access(const char *path, int mask);
static int sfs_readlink(const char *path, char *buf, size_t size);
static int sfs_mkdir(const char *path, mode_t mode);
static int sfs_unlink(const char *path);
static int sfs_rmdir(const char *path);
static int sfs_rename(const char *from, const char *to);
static int sfs_chmod(const char *path, mode_t mode);
static int sfs_chown(const char *path, uid_t uid, gid_t gid);
//static int sfs_utimens(const char *path, const struct timespec ts[2]);
static int sfs_open(const char *path, struct fuse_file_info *fi);
static int sfs_create(const char *path, mode_t mode, struct fuse_file_info *fi);
static int sfs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi);
static int sfs_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi);
static void sfs_destroy(void* unused);
static void* sfs_init(fuse_conn_info *conn);
//static void fuseInitialisedSignalHandler(int sig, siginfo_t *siginfo, void *context);
//static void fuseEndedSignalHandler(int sig, siginfo_t *siginfo, void *context);
static void fuseSignalHandler(int sig, siginfo_t *siginfo, void *context);
static void attachVirtualDisk();

//VirtualDisc* FuseService::virtualDisc = NULL;
VirtualStorage* FuseService::virtual_storage_ = NULL;
uint64 FuseService::capacity_ = 0;
FuseServiceDelegate* FuseService::delegate_ = NULL;
pid_t FuseService::fuse_proc_pid_ = 0;
bool FuseService::fuse_mounted_ = false;
QString FuseService::mount_point_ = QString("");
QProcess* FuseService::child_process_ = NULL;

// =============================================================================
//      MAIN
// =============================================================================


int FuseService::Init(VirtualStorage *virtual_storage) {
  //FuseService::virtualDisc = virtualDisc;
  virtual_storage_ = virtual_storage;

  capacity_ = virtual_storage_->GetUsableCapacity();

  // fuse_operations struct initialization
  stegofs_ops.init        = sfs_init;
  stegofs_ops.getattr     = sfs_getattr;
  stegofs_ops.access		= sfs_access;
  stegofs_ops.readlink	= sfs_readlink;
  stegofs_ops.readdir     = sfs_readdir;
  stegofs_ops.mkdir		= sfs_mkdir;
  stegofs_ops.unlink		= sfs_unlink;
  stegofs_ops.rmdir		= sfs_rmdir;
  stegofs_ops.rename		= sfs_rename;
  stegofs_ops.chmod		= sfs_chmod;
  stegofs_ops.chown		= sfs_chown;
  //stegofs_ops.truncate	= sfs_truncate;
  //stegofs_ops.utimens     = sfs_utimens;
  stegofs_ops.create		= sfs_create;
  stegofs_ops.open		= sfs_open;
  stegofs_ops.read		= sfs_read;
  stegofs_ops.write		= sfs_write;
  stegofs_ops.destroy     = sfs_destroy;

  return 0;
}

//int FuseService::mount(const char* mount_point_)
int FuseService::MountFuse(QString mount_point) {
  // bbfs doesn't do any access checking on its own (the comment
  // blocks in fuse.h mention some of the functions that need
  // accesses checked -- but note there are other functions, like
  // chown(), that also need checking!).  Since running bbfs as root
  // will therefore open Metrodome-sized holes in the system
  // security, we'll check if root is trying to mount the filesystem
  // and refuse if it is.  The somewhat smaller hole of an ordinary
  // user doing it with the allow_other flag is still there because
  // I don't want to parse the options string.
  //if ((getuid() == 0) || (geteuid() == 0)) {
  //   fprintf(stderr, "Running BBFS as root opens unnacceptable security holes\n");
  //   return 1;
  //}

  mount_point_ = mount_point;


  char *argv[10];
  memset(argv, 0, 10 * sizeof(char*));

  char *mnt_pt = new (nothrow) char[mount_point_.size() + 1];
  if (!mnt_pt) {
    return -1;
  }
  strcpy(mnt_pt, mount_point_.toStdString().c_str());

  LOG_INFO("mount point: " << mnt_pt);

  char debug[] = "-d";


  argv[0] = (char*)"fuse";
  argv[1] = mnt_pt;
  argv[2] = debug;
  argv[3] = (char*)"-o";
  argv[4] = (char*)"allow_other";
  argv[5] = NULL;
  int argc = 5;
  //TODO: add nobrowse param

  /*
    argv[0] = "fuse";
    argv[1] = mnt_pt;
    argv[2] = "-o";
    argv[3] = "allow_other";
    argv[4] = NULL;
    int argc = 4;
*/


  fuse_mounted_ = false;

  // turn over control to fuse
  LOG_INFO("calling fuse_main");
  int fuse_stat;

  pid_t pid = fork();

  if (pid >= 0) {
    LOG_INFO("fork succeed");
    if (pid == 0) {

      //freopen("/dev/null", "w", stderr);

      LOG_INFO("fuse pid: " << getpid());

      // TODO: add signal handlers to
      //

      fuse_stat = fuse_main(argc, argv, &stegofs_ops, NULL);
      LOG_INFO("fuse_main returned " << fuse_stat);
      _exit(fuse_stat);
    } else {
      //LOG_INFO("main pid: " << getpid());
      fuse_proc_pid_ = pid;


      // TODO: toto presunut este nad fork a v pid=0 vyclearovat
      struct sigaction act;
      memset (&act, '\0', sizeof(act));
      /* Use the sa_sigaction field because the handles has two additional parameters */
      act.sa_sigaction = &fuseSignalHandler;
      /* The SA_SIGINFO flag tells sigaction() to use the sa_sigaction field, not sa_handler. */
      act.sa_flags = SA_SIGINFO;
      if (sigaction(SIGUSR1, &act, NULL) < 0) {
        LOG_ERROR("sigaction failed");
      }
      if (sigaction(SIGUSR2, &act, NULL) < 0) {
        LOG_ERROR("sigaction failed");
      }
      /*
            if (sigaction(SIGCHLD, &act, NULL) < 0) {
                LOG_ERROR("sigaction failed");
            }
            */
      /*
            signal(SIGUSR1, fuseInitialisedSignalHandler);
            signal(SIGUSR2, fuseEndedSignalHandler);
            signal(SIGCHLD, fuseSigChldSignalHandler);
            */
      LOG_INFO("wait for fuse pid");
      waitpid(pid, NULL, 0);
      LOG_INFO("wait for fuse pid ended");
      fuse_mounted_ = false;
      if (delegate_ != NULL) {
        delegate_->fuseUnmounted();
      }
      return 0;
    }
  } else {
    LOG_INFO("fork failed");
    return -1;
  }

  //fuse_stat = fuse_main_realargc, argv, &stegofs_ops, 0. NULL);


  //return fuse_stat;
}


// =============================================================================
//      FUSE CALLBACK METHODS
// =============================================================================

static int sfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t, struct fuse_file_info *) {// path, buf, filler, off_t offset, fi

  if (strcmp(path, "/") != 0) /* We only recognize the root directory. */
    return -ENOENT;

  if (!FuseService::fuse_mounted_) {
    FuseService::fuse_mounted_ = true;
    kill(getppid(), SIGUSR1);
  }

  filler(buf, ".", NULL, 0);           /* Current directory (.)  */
  filler(buf, "..", NULL, 0);          /* Parent directory (..)  */
  filler(buf, file_path + 1, NULL, 0); /* The only file we have. */

  return 0;
}

static int sfs_getattr(const char *path, struct stat *stbuf) {
  memset(stbuf, 0, sizeof(struct stat));

  if (strcmp(path, "/") == 0) { /* The root directory of our file system. */
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 3;
  } else if (strcmp(path, file_path) == 0) { /* The only file we have. */
    stbuf->st_mode = S_IFREG | 0777;
    stbuf->st_nlink = 1;
    stbuf->st_size = FuseService::capacity_;
    //stbuf->st_blocks = (FuseService::capacity_/512) + 1;
  } else /* We reject everything else. */
    return -ENOENT;

  return 0;
}

static int sfs_access(const char *, int ) { // const char * path, int mask
  return 0;
}

static int sfs_readlink(const char *, char *, size_t ) { // const char *path, char *buf, size_t size
  return -ENOSYS;
}

static int sfs_mkdir(const char *, mode_t ) { // const char *path, mode_t mode
  return -EROFS;
}

static int sfs_unlink(const char *) { // const char *path
  return 0;
  //return -EROFS;
}

static int sfs_rmdir(const char *) { // const char *path
  return -EROFS;
}
static int sfs_rename(const char *, const char *) { // const char *from, const char *to
  return -EROFS;
}

static int sfs_chmod(const char *, mode_t ) { // const char *path, mode_t mode
  return 0;
}
static int sfs_chown(const char *, uid_t , gid_t ) { // const char *path, uid_t uid, gid_t gid
  return 0;
}

//static int sfs_utimens(const char *path, const struct timespec ts[2])
//{
//	return 0;
//}

static int sfs_open(const char *, struct fuse_file_info *) // const chr *path, struct fuse_file_info *fi
{

  //if (strcmp(path, file_path) != 0) /* We only recognize one file. */
  //    return -ENOENT;

  //if ((fi->flags & O_ACCMODE) != O_RDONLY) /* Only reading allowed. */
  //    return -EACCES;

  return 0;
}

static int sfs_create(const char *path, mode_t, struct fuse_file_info *fi) { // path, mode_t mode, fi
  return sfs_open(path, fi);
}


static int sfs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *) { // struct fuse_file_info *fi

  if (strcmp(path, file_path) != 0)
    return -ENOENT;

  uint64 offset64 = offset;
  uint64 size64 = size;

  if (offset64 >= FuseService::capacity_) /* Trying to read past the end of file. */ {
    LOG_ERROR("fuse service: offset+size > capacity_... offset:"
              << offset << ", size:" << size << ", cap:"
              << FuseService::capacity_);
    return 0;
  }

  if (offset64 + size64 > FuseService::capacity_) /* Trim the read to the file size. */ {
    LOG_ERROR("fuse service: offset+size > capacity_... offset:"
              << offset << ", size:" << size << ", cap:"
              << FuseService::capacity_);
    size64 = FuseService::capacity_ - offset64;
  }

  //int err = FuseService::virtualDisc->write((uint8*)buf, (uint32)size, offset);
  int err = FuseService::virtual_storage_->Read(offset64,
                                                static_cast<uint32>(size64),
                                                (uint8*)buf);

  if (err) {
    // TODO: treba nejak rozumne prelozit errory
    //LOG_ERROR("write error: " << err << ", offset: " << offset << ", size: " << size);
    return ENOMEM;
  } else {
    return static_cast<int>(size);
  }
}

static int sfs_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *) { // struct fuse_file_info *fi

  if (strcmp(path, file_path) != 0)
    return -ENOENT;

  uint64 offset64 = offset;
  uint64 size64 = size;

  if (offset64 >= FuseService::capacity_) /* Trying to read past the end of file. */ {
    LOG_ERROR("fuse service: offset > capacity_... offset:"
              << offset << ", size:" << size << ", cap:"
              << FuseService::capacity_);
    return 0;
  }

  if (offset64 + size64 > FuseService::capacity_) /* Trim the read to the file size. */ {
    LOG_ERROR("fuse service: offset+size > capacity_... offset:"
              << offset << ", size:" << size << ", cap:"
              << FuseService::capacity_);
    size64 = FuseService::capacity_ - offset64;
  }

  //int err = FuseService::virtualDisc->write((uint8*)buf, (uint32)size, offset);
  int err = FuseService::virtual_storage_->Write(offset64,
                                                 static_cast<uint32>(size64),
                                                 (uint8*)buf);

  if (err) {
    // TODO: treba nejak rozumne prelozit errory
    //LOG_ERROR("write error: " << err << ", offset: " << offset << ", size: " << size);
    return ENOMEM;
  } else {
    return static_cast<int>(size);
  }
}

static void* sfs_init(struct fuse_conn_info *) {
  LOG_DEBUG("SFS_INIT CALLED");
  //kill(getppid(), SIGUSR1);
  return NULL;
}

static void sfs_destroy(void*) {
  LOG_DEBUG("SFS_DESTROY CALLED @pid: " << getpid());
  LOG_DEBUG("signaling parrent with id: " << getppid());
  //kill(getppid(), SIGUSR2);
  kill(getppid(), SIGUSR2);
}

static void fuseInitialisedSignalHandler() {
  LOG_INFO("sig1 handler called");

  /*
    if (FuseService::delegate_ != NULL) {
        FuseService::delegate_->fuse_mounted_(true);
    }
    */

  QtConcurrent::run(attachVirtualDisk);

}

//static void fuseEndedSignalHandler()
//{
//LOG_DEBUG("finished signal handler @pid: " << getpid() << ", with signal:" << signum);
/*
    if (FuseService::destroyCallback != NULL) {
        FuseService::destroyCallback();
    }
    */
//}

static void fuseSigChldSignalHandler() {
  LOG_DEBUG("sigchld called");
  if (FuseService::fuse_proc_pid_) {

    LOG_DEBUG("waitpid called");
    waitpid(FuseService::fuse_proc_pid_, NULL, 0);
    LOG_DEBUG("waitpid ended");

  }

  FuseService::fuse_mounted_ = false;
  if (FuseService::delegate_ != NULL) {
    FuseService::delegate_->fuseUnmounted();
  }

}


static void attachVirtualDisk() {

  if (FuseService::child_process_) {
    // TODO: do sth
  }

  QString virtual_disk_file_path =
      QDir(FuseService::mount_point_).absoluteFilePath(
        QString(FuseService::virtual_file_name_));
  LOG_INFO("VDF PATH: " << virtual_disk_file_path.toStdString());
  LOG_INFO("file exists: " <<
           QFile().exists(QDir(FuseService::mount_point_).absoluteFilePath(
                            QString(FuseService::virtual_file_name_))));

  QProcess *attach = new QProcess();
  FuseService::child_process_ = attach;
  QStringList args;
  args.push_back("attach");
  args.push_back(virtual_disk_file_path);
  args.push_back("-imagekey");
  args.push_back("diskimage-class=CRawDiskImage");
  //args.push_back("-nomount");

  LOG_INFO("calling attach");
  //QObject::connect(attach, SIGNAL(finished(int,QProcess::ExitStatus)), this, SLOT(onAttachFinished(int,QProcess::ExitStatus)));
  attach->setProcessChannelMode(QProcess::MergedChannels);
  attach->start("hdiutil", args);
  attach->waitForFinished(60000);
  //int retval = QProcess::execute("hdiutil", args);
  //LOG_INFO("returned: " << retval);


  //attach->readAll()
  QByteArray output = attach->readAllStandardOutput();
  QString diskName = QString(output).trimmed();
  LOG_INFO("trimmed:" << diskName.toStdString());
  if (diskName.startsWith("/dev/")) {
    LOG_INFO("disk name ok");
  }
  LOG_INFO("exit code: " << attach->exitCode());
  LOG_INFO("exit status: " << attach->exitStatus());

  // exit code 1 a "hdiutil: attach failed - no mountable file systems"
  // exit code 0 a /dev/diskX - ked nomount

  /*
    LOG_INFO("byte array len: " << output.size());
    LOG_INFO("output: o'" << QString(output).toStdString() << "'o");
    QString outStr = QString(output);
    QStringList outStringList = outStr.split("\n");
    LOG_INFO("lines: " << outStringList.size());
    if (outStringList.size()>0) {
        LOG_INFO("1.: l'" << outStringList.at(0).toStdString() << "'l");
        if (outStringList.at(0).startsWith("/dev/disk")) {
            LOG_INFO("dobry riadok muehehe");
            QString cool = outStringList.at(0).trimmed();
        }
    }
    output = attach->readAllStandardError();
    LOG_INFO("errout: e'" << QString(output).toStdString() << "'e");
    LOG_INFO("exit code: " << attach->exitCode());
    LOG_INFO("exit status: " << attach->exitStatus());
    */

}

static void fuseSignalHandler(int sig, siginfo_t *siginfo, void *) {
  LOG_INFO("SIGNAL HANDLER CALLED sig: " << sig << ", pid: "
           << siginfo->si_pid << ", fusepid: " <<
           FuseService::fuse_proc_pid_ << ", status: " << siginfo->si_status);

  if (siginfo->si_pid == FuseService::fuse_proc_pid_) {

    switch (sig) {
      case SIGUSR1:
        QtConcurrent::run(fuseInitialisedSignalHandler);
        break;
      case SIGCHLD:
        fuseSigChldSignalHandler();
        break;
    }

    return;
  }

  if (FuseService::child_process_) {
    if (siginfo->si_pid == FuseService::child_process_->pid()) {
      LOG_INFO("program: " <<
               FuseService::child_process_->program().toStdString());
      QByteArray output = FuseService::child_process_->readAllStandardOutput();
      LOG_INFO("output: " << QString(output).toStdString());
      output = FuseService::child_process_->readAllStandardError();
      LOG_INFO("std err output: " << QString(output).toStdString());
      LOG_INFO("exit code: " << FuseService::child_process_->exitCode());
      LOG_INFO("exit status: " << FuseService::child_process_->exitStatus());
    }
  }

}

int FuseService::unmountFuse(QString mount_point_) {
  LOG_INFO("unmounting: " << mount_point_.toStdString());
  //return umount(mount_point_.toStdString().c_str());
  return 1;
}

#endif