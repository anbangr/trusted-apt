// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: cdromutl.cc,v 1.12 2001/02/20 07:03:17 jgg Exp $
/* ######################################################################
   
   CDROM Utilities - Some functions to manipulate CDROM mounts.
   
   These are here for the cdrom method and apt-cdrom.
   
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include<config.h>

#include <apt-pkg/cdromutl.h>
#include <apt-pkg/error.h>
#include <apt-pkg/md5.h>
#include <apt-pkg/fileutl.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/strutl.h>

#include <sys/wait.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>

#include <apti18n.h>
									/*}}}*/

using std::string;

// IsMounted - Returns true if the mount point is mounted		/*{{{*/
// ---------------------------------------------------------------------
/* This is a simple algorithm that should always work, we stat the mount point
   and the '..' file in the mount point and see if they are on the same device.
   By definition if they are the same then it is not mounted. This should 
   account for symlinked mount points as well. */
bool IsMounted(string &Path)
{
   if (Path.empty() == true)
      return false;
   
   // Need that trailing slash for directories
   if (Path[Path.length() - 1] != '/')
      Path += '/';
   
   /* First we check if the path is actualy mounted, we do this by
      stating the path and the previous directory (carefull of links!)
      and comparing their device fields. */
   struct stat Buf,Buf2;
   if (stat(Path.c_str(),&Buf) != 0 || 
       stat((Path + "../").c_str(),&Buf2) != 0)
      return _error->Errno("stat",_("Unable to stat the mount point %s"),Path.c_str());

   if (Buf.st_dev == Buf2.st_dev)
      return false;
   return true;
}
									/*}}}*/
// UnmountCdrom - Unmount a cdrom					/*{{{*/
// ---------------------------------------------------------------------
/* Forking umount works much better than the umount syscall which can 
   leave /etc/mtab inconsitant. We drop all messages this produces. */
bool UnmountCdrom(string Path)
{
   if (IsMounted(Path) == false)
      return true;

   for (int i=0;i<3;i++)
   {
   
      int Child = ExecFork();

      // The child
      if (Child == 0)
      {
	 // Make all the fds /dev/null
	 for (int I = 0; I != 3; I++)
	    dup2(open("/dev/null",O_RDWR),I);

	 if (_config->Exists("Acquire::cdrom::"+Path+"::UMount") == true)
	 {
	    if (system(_config->Find("Acquire::cdrom::"+Path+"::UMount").c_str()) != 0)
	       _exit(100);
	    _exit(0);	 	 
	 }
	 else
	 {
	    const char *Args[10];
	    Args[0] = "umount";
	    Args[1] = Path.c_str();
	    Args[2] = 0;
	    execvp(Args[0],(char **)Args);      
	    _exit(100);
	 }      
      }

      // if it can not be umounted, give it a bit more time
      // this can happen when auto-mount magic or fs/cdrom prober attack
      if (ExecWait(Child,"umount",true) == true)
	 return true;
      sleep(1);
   }

   return false;
}
									/*}}}*/
// MountCdrom - Mount a cdrom						/*{{{*/
// ---------------------------------------------------------------------
/* We fork mount and drop all messages */
bool MountCdrom(string Path, string DeviceName)
{
   if (IsMounted(Path) == true)
      return true;
   
   int Child = ExecFork();

   // The child
   if (Child == 0)
   {
      // Make all the fds /dev/null
      for (int I = 0; I != 3; I++)
	 dup2(open("/dev/null",O_RDWR),I);
      
      if (_config->Exists("Acquire::cdrom::"+Path+"::Mount") == true)
      {
	 if (system(_config->Find("Acquire::cdrom::"+Path+"::Mount").c_str()) != 0)
	    _exit(100);
	 _exit(0);	 
      }
      else
      {
	 const char *Args[10];
	 Args[0] = "mount";
	 if (DeviceName == "") 
	 {
	    Args[1] = Path.c_str();
	    Args[2] = 0;
	 } else {
	    Args[1] = DeviceName.c_str();
	    Args[2] = Path.c_str();
	    Args[3] = 0;
	 }
	 execvp(Args[0],(char **)Args);      
	 _exit(100);
      }      
   }

   // Wait for mount
   return ExecWait(Child,"mount",true);
}
									/*}}}*/
// IdentCdrom - Generate a unique string for this CD			/*{{{*/
// ---------------------------------------------------------------------
/* We convert everything we hash into a string, this prevents byte size/order
   from effecting the outcome. */
bool IdentCdrom(string CD,string &Res,unsigned int Version)
{
   MD5Summation Hash;
   bool writable_media = false;

   // if we are on a writable medium (like a usb-stick) that is just
   // used like a cdrom don't use "." as it will constantly change,
   // use .disk instead
   if (access(CD.c_str(), W_OK) == 0 && DirectoryExists(CD+string("/.disk"))) 
   {
      writable_media = true;
      CD = CD.append("/.disk");
      if (_config->FindB("Debug::aptcdrom",false) == true)
         std::clog << "Found writable cdrom, using alternative path: " << CD
                   << std::endl;
   }

   string StartDir = SafeGetCWD();
   if (chdir(CD.c_str()) != 0)
      return _error->Errno("chdir",_("Unable to change to %s"),CD.c_str());
   
   DIR *D = opendir(".");
   if (D == 0)
      return _error->Errno("opendir",_("Unable to read %s"),CD.c_str());
      
   /* Run over the directory, we assume that the reader order will never
      change as the media is read-only. In theory if the kernel did
      some sort of wacked caching this might not be true.. */
   char S[300];
   for (struct dirent *Dir = readdir(D); Dir != 0; Dir = readdir(D))
   {
      // Skip some files..
      if (strcmp(Dir->d_name,".") == 0 ||
	  strcmp(Dir->d_name,"..") == 0)
	 continue;

      if (Version <= 1)
      {
	 sprintf(S,"%lu",(unsigned long)Dir->d_ino);
      }
      else
      {
	 struct stat Buf;
	 if (stat(Dir->d_name,&Buf) != 0)
	    continue;
	 sprintf(S,"%lu",(unsigned long)Buf.st_mtime);
      }
      
      Hash.Add(S);
      Hash.Add(Dir->d_name);
   };
   
   if (chdir(StartDir.c_str()) != 0) {
      _error->Errno("chdir",_("Unable to change to %s"),StartDir.c_str());
      closedir(D);
      return false;
   }
   closedir(D);
   
   // Some stats from the fsys
   if (_config->FindB("Debug::identcdrom",false) == false)
   {
      struct statvfs Buf;
      if (statvfs(CD.c_str(),&Buf) != 0)
	 return _error->Errno("statfs",_("Failed to stat the cdrom"));

      // We use a kilobyte block size to advoid overflow
      if (writable_media)
      {
         sprintf(S,"%lu",(long)(Buf.f_blocks*(Buf.f_bsize/1024)));
      } else {
         sprintf(S,"%lu %lu",(long)(Buf.f_blocks*(Buf.f_bsize/1024)),
                 (long)(Buf.f_bfree*(Buf.f_bsize/1024)));
      }
      Hash.Add(S);
      sprintf(S,"-%u",Version);
   }
   else
      sprintf(S,"-%u.debug",Version);
   
   Res = Hash.Result().Value() + S;
   return true;   
}
									/*}}}*/

// FindMountPointForDevice - Find mountpoint for the given device	/*{{{*/
string FindMountPointForDevice(const char *devnode)
{
   char buf[255];
   char *out[10];
   int i=0;

   // this is the order that mount uses as well
   const char *mount[] = { "/etc/mtab", 
                           "/proc/mount", 
                           NULL };

   for (i=0; mount[i] != NULL; i++) {
      if (FileExists(mount[i])) {
         FILE *f=fopen(mount[i], "r");
         while ( fgets(buf, sizeof(buf), f) != NULL) {
            if (strncmp(buf, devnode, strlen(devnode)) == 0) {
               if(TokSplitString(' ', buf, out, 10))
               {
                  fclose(f);
                  // unescape the \0XXX chars in the path
                  string mount_point = out[1];
                  return DeEscapeString(mount_point);
               }
            }
         }
         fclose(f);
      }
   }
   
   return string();
}
									/*}}}*/
