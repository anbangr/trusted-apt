// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: multicompress.cc,v 1.4 2003/02/10 07:34:41 doogie Exp $
/* ######################################################################

   MultiCompressor

   This class is very complicated in order to optimize for the common
   case of its use, writing a large set of compressed files that are 
   different from the old set. It spawns off compressors in parallel
   to maximize compression throughput and has a separate task managing
   the data going into the compressors.
   
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include <config.h>

#include <apt-pkg/fileutl.h>
#include <apt-pkg/strutl.h>
#include <apt-pkg/error.h>
#include <apt-pkg/md5.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>
#include <iostream>

#include "multicompress.h"
#include <apti18n.h>
									/*}}}*/

using namespace std;


// MultiCompress::MultiCompress - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* Setup the file outputs, compression modes and fork the writer child */
MultiCompress::MultiCompress(string const &Output,string const &Compress,
			     mode_t const &Permissions,bool const &Write) :
			Permissions(Permissions)
{
   Outputs = 0;
   Outputter = -1;
   Input = 0;
   UpdateMTime = 0;

   /* Parse the compression string, a space separated lists of compresison
      types */
   string::const_iterator I = Compress.begin();
   for (; I != Compress.end();)
   {
      for (; I != Compress.end() && isspace(*I); ++I);
      
      // Grab a word
      string::const_iterator Start = I;
      for (; I != Compress.end() && !isspace(*I); ++I);

      // Find the matching compressor
      std::vector<APT::Configuration::Compressor> Compressors = APT::Configuration::getCompressors();
      std::vector<APT::Configuration::Compressor>::const_iterator Comp = Compressors.begin();
      for (; Comp != Compressors.end(); ++Comp)
	 if (stringcmp(Start,I,Comp->Name.c_str()) == 0)
	    break;

      // Hmm.. unknown.
      if (Comp == Compressors.end())
      {
	 _error->Warning(_("Unknown compression algorithm '%s'"),string(Start,I).c_str());
	 continue;
      }
      
      // Create and link in a new output 
      Files *NewOut = new Files;
      NewOut->Next = Outputs;
      Outputs = NewOut;
      NewOut->CompressProg = *Comp;
      NewOut->Output = Output+Comp->Extension;
      
      struct stat St;
      if (stat(NewOut->Output.c_str(),&St) == 0)
	 NewOut->OldMTime = St.st_mtime;
      else
	 NewOut->OldMTime = 0;
   }
   
   if (Write == false)
       return;
       
   /* Open all the temp files now so we can report any errors. File is 
      made unreable to prevent people from touching it during creating. */
   for (Files *I = Outputs; I != 0; I = I->Next)
      I->TmpFile.Open(I->Output + ".new", FileFd::WriteOnly | FileFd::Create | FileFd::Empty, FileFd::Extension, 0600);
   if (_error->PendingError() == true)
      return;

   if (Outputs == 0)
   {
      _error->Error(_("Compressed output %s needs a compression set"),Output.c_str());
      return;
   }

   Start();
}
									/*}}}*/
// MultiCompress::~MultiCompress - Destructor				/*{{{*/
// ---------------------------------------------------------------------
/* Just erase the file linked list. */
MultiCompress::~MultiCompress()
{
   Die();
   
   for (; Outputs != 0;)
   {
      Files *Tmp = Outputs->Next;
      delete Outputs;
      Outputs = Tmp;
   }   
}
									/*}}}*/
// MultiCompress::GetStat - Get stat information for compressed files	/*{{{*/
// ---------------------------------------------------------------------
/* This checks each compressed file to make sure it exists and returns
   stat information for a random file from the collection. False means
   one or more of the files is missing. */
bool MultiCompress::GetStat(string const &Output,string const &Compress,struct stat &St)
{
   /* Parse the compression string, a space separated lists of compresison
      types */
   string::const_iterator I = Compress.begin();
   bool DidStat = false;
   for (; I != Compress.end();)
   {
      for (; I != Compress.end() && isspace(*I); ++I);
      
      // Grab a word
      string::const_iterator Start = I;
      for (; I != Compress.end() && !isspace(*I); ++I);

      // Find the matching compressor
      std::vector<APT::Configuration::Compressor> Compressors = APT::Configuration::getCompressors();
      std::vector<APT::Configuration::Compressor>::const_iterator Comp = Compressors.begin();
      for (; Comp != Compressors.end(); ++Comp)
	 if (stringcmp(Start,I,Comp->Name.c_str()) == 0)
	    break;

      // Hmm.. unknown.
      if (Comp == Compressors.end())
	 continue;

      string Name = Output+Comp->Extension;
      if (stat(Name.c_str(),&St) != 0)
	 return false;
      DidStat = true;
   }   
   return DidStat;
}
									/*}}}*/
// MultiCompress::Start - Start up the writer child			/*{{{*/
// ---------------------------------------------------------------------
/* Fork a child and setup the communication pipe. */
bool MultiCompress::Start()
{
   // Create a data pipe
   int Pipe[2] = {-1,-1};
   if (pipe(Pipe) != 0)
      return _error->Errno("pipe",_("Failed to create IPC pipe to subprocess"));
   for (int I = 0; I != 2; I++)
      SetCloseExec(Pipe[I],true);
   
   // The child..
   Outputter = fork();
   if (Outputter == 0)
   {
      close(Pipe[1]);
      Child(Pipe[0]);
      if (_error->PendingError() == true)
      {
	 _error->DumpErrors();
	 _exit(100);
      }      
      _exit(0);
   };

   close(Pipe[0]);
   Input = fdopen(Pipe[1],"w");
   if (Input == 0)
      return _error->Errno("fdopen",_("Failed to create FILE*"));
   
   if (Outputter == -1)
      return _error->Errno("fork",_("Failed to fork"));   
   return true;
}
									/*}}}*/
// MultiCompress::Die - Clean up the writer				/*{{{*/
// ---------------------------------------------------------------------
/* */
bool MultiCompress::Die()
{
   if (Input == 0)
      return true;
   
   fclose(Input);
   Input = 0;
   bool Res = ExecWait(Outputter,_("Compress child"),false);
   Outputter = -1;
   return Res;
}
									/*}}}*/
// MultiCompress::Finalize - Finish up writing				/*{{{*/
// ---------------------------------------------------------------------
/* This is only necessary for statistics reporting. */
bool MultiCompress::Finalize(unsigned long long &OutSize)
{
   OutSize = 0;
   if (Input == 0 || Die() == false)
      return false;
   
   time_t Now;
   time(&Now);
   
   // Check the mtimes to see if the files were replaced.
   bool Changed = false;
   for (Files *I = Outputs; I != 0; I = I->Next)
   {
      struct stat St;
      if (stat(I->Output.c_str(),&St) != 0)
	 return  _error->Error(_("Internal error, failed to create %s"),
			       I->Output.c_str());
      
      if (I->OldMTime != St.st_mtime)
	 Changed = true;
      else
      {
	 // Update the mtime if necessary
	 if (UpdateMTime > 0 && 
	     (Now - St.st_mtime > (signed)UpdateMTime || St.st_mtime > Now))
	 {
	    struct utimbuf Buf;
	    Buf.actime = Buf.modtime = Now;
	    utime(I->Output.c_str(),&Buf);
	    Changed = true;
	 }	     
      }
      
      // Force the file permissions
      if (St.st_mode != Permissions)
	 chmod(I->Output.c_str(),Permissions);
      
      OutSize += St.st_size;
   }
   
   if (Changed == false)
      OutSize = 0;
   
   return true;
}
									/*}}}*/
// MultiCompress::OpenOld - Open an old file				/*{{{*/
// ---------------------------------------------------------------------
/* This opens one of the original output files, possibly decompressing it. */
bool MultiCompress::OpenOld(FileFd &Fd)
{
   Files *Best = Outputs;
   for (Files *I = Outputs; I != 0; I = I->Next)
      if (Best->CompressProg.Cost > I->CompressProg.Cost)
	 Best = I;

   // Open the file
   return Fd.Open(Best->Output, FileFd::ReadOnly, FileFd::Extension);
}
									/*}}}*/
// MultiCompress::Child - The writer child				/*{{{*/
// ---------------------------------------------------------------------
/* The child process forks a bunch of compression children and takes 
   input on FD and passes it to all the compressor child. On the way it
   computes the MD5 of the raw data. After this the raw data in the 
   original files is compared to see if this data is new. If the data
   is new then the temp files are renamed, otherwise they are erased. */
bool MultiCompress::Child(int const &FD)
{
   /* Okay, now we just feed data from FD to all the other FDs. Also
      stash a hash of the data to use later. */
   SetNonBlock(FD,false);
   unsigned char Buffer[32*1024];
   unsigned long long FileSize = 0;
   MD5Summation MD5;
   while (1)
   {
      WaitFd(FD,false);
      int Res = read(FD,Buffer,sizeof(Buffer));
      if (Res == 0)
	 break;
      if (Res < 0)
	 continue;

      MD5.Add(Buffer,Res);
      FileSize += Res;
      for (Files *I = Outputs; I != 0; I = I->Next)
      {
	 if (I->TmpFile.Write(Buffer, Res) == false)
	 {
	    _error->Errno("write",_("IO to subprocess/file failed"));
	    break;
	 }
      }      
   }   

   if (_error->PendingError() == true)
      return false;
   
   /* Now we have to copy the files over, or erase them if they
      have not changed. First find the cheapest decompressor */
   bool Missing = false;
   for (Files *I = Outputs; I != 0; I = I->Next)
   {
      if (I->OldMTime == 0)
      {
	 Missing = true;
	 break;
      }            
   }
   
   // Check the MD5 of the lowest cost entity.
   while (Missing == false)
   {
      FileFd CompFd;
      if (OpenOld(CompFd) == false)
      {
	 _error->Discard();
	 break;
      }

      // Compute the hash
      MD5Summation OldMD5;
      unsigned long long NewFileSize = 0;
      while (1)
      {
	 unsigned long long Res = 0;
	 if (CompFd.Read(Buffer,sizeof(Buffer), &Res) == false)
	    return _error->Errno("read",_("Failed to read while computing MD5"));
	 if (Res == 0)
	    break;
	 NewFileSize += Res;
	 OldMD5.Add(Buffer,Res);
      }
      CompFd.Close();

      // Check the hash
      if (OldMD5.Result() == MD5.Result() &&
	  FileSize == NewFileSize)
      {
	 for (Files *I = Outputs; I != 0; I = I->Next)
	 {
	    I->TmpFile.Close();
	    if (unlink(I->TmpFile.Name().c_str()) != 0)
	       _error->Errno("unlink",_("Problem unlinking %s"),
			     I->TmpFile.Name().c_str());
	 }
	 return !_error->PendingError();
      }      
      break;
   }

   // Finalize
   for (Files *I = Outputs; I != 0; I = I->Next)
   {
      // Set the correct file modes
      fchmod(I->TmpFile.Fd(),Permissions);
      
      if (rename(I->TmpFile.Name().c_str(),I->Output.c_str()) != 0)
	 _error->Errno("rename",_("Failed to rename %s to %s"),
		       I->TmpFile.Name().c_str(),I->Output.c_str());
      I->TmpFile.Close();
   }
   
   return !_error->PendingError();
}
									/*}}}*/

