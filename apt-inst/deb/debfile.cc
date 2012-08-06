// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: debfile.cc,v 1.3.2.1 2004/01/16 18:58:50 mdz Exp $
/* ######################################################################

   Debian Archive File (.deb)
   
   .DEB archives are AR files containing two tars and an empty marker
   member called 'debian-binary'. The two tars contain the meta data and
   the actual archive contents. Thus this class is a very simple wrapper
   around ar/tar to simply extract the right tar files.
   
   It also uses the deb package list parser to parse the control file 
   into the cache.
   
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include<config.h>

#include <apt-pkg/database.h>
#include <apt-pkg/debfile.h>
#include <apt-pkg/extracttar.h>
#include <apt-pkg/error.h>
#include <apt-pkg/deblistparser.h>
#include <apt-pkg/aptconfiguration.h>

#include <sys/stat.h>
#include <unistd.h>
#include <apti18n.h>
									/*}}}*/

// DebFile::debDebFile - Constructor					/*{{{*/
// ---------------------------------------------------------------------
/* Open the AR file and check for consistency */
debDebFile::debDebFile(FileFd &File) : File(File), AR(File)
{
   if (_error->PendingError() == true)
      return;

   if (!CheckMember("debian-binary")) {
      _error->Error(_("This is not a valid DEB archive, missing '%s' member"), "debian-binary");
      return;
   }

   if (!CheckMember("control.tar.gz")) {
      _error->Error(_("This is not a valid DEB archive, missing '%s' member"), "control.tar.gz");
      return;
   }

   if (!CheckMember("data.tar.gz") &&
       !CheckMember("data.tar.bz2") &&
       !CheckMember("data.tar.lzma") &&
       !CheckMember("data.tar.xz")) {
      // FIXME: add data.tar.xz here - adding it now would require a Translation round for a very small gain
      _error->Error(_("This is not a valid DEB archive, it has no '%s', '%s' or '%s' member"), "data.tar.gz", "data.tar.bz2", "data.tar.lzma");
      return;
   }
}
									/*}}}*/
// DebFile::CheckMember - Check if a named member is in the archive	/*{{{*/
// ---------------------------------------------------------------------
/* This is used to check for a correct deb and to give nicer error messages
   for people playing around. */
bool debDebFile::CheckMember(const char *Name)
{
   if (AR.FindMember(Name) == 0)
      return false;
   return true;
}
									/*}}}*/
// DebFile::GotoMember - Jump to a Member				/*{{{*/
// ---------------------------------------------------------------------
/* Jump in the file to the start of a named member and return the information
   about that member. The caller can then read from the file up to the 
   returned size. Note, since this relies on the file position this is
   a destructive operation, it also changes the last returned Member
   structure - so don't nest them! */
const ARArchive::Member *debDebFile::GotoMember(const char *Name)
{
   // Get the archive member and positition the file
   const ARArchive::Member *Member = AR.FindMember(Name);
   if (Member == 0)
   {
      return 0;
   }
   if (File.Seek(Member->Start) == false)
      return 0;
      
   return Member;
}
									/*}}}*/
// DebFile::ExtractControl - Extract Control information		/*{{{*/
// ---------------------------------------------------------------------
/* Extract the control information into the Database's temporary 
   directory. */
bool debDebFile::ExtractControl(pkgDataBase &DB)
{
   // Get the archive member and positition the file
   const ARArchive::Member *Member = GotoMember("control.tar.gz");
   if (Member == 0)
      return false;
      
   // Prepare Tar
   ControlExtract Extract;
   ExtractTar Tar(File,Member->Size,"gzip");
   if (_error->PendingError() == true)
      return false;
   
   // Get into the temporary directory
   std::string Cwd = SafeGetCWD();
   std::string Tmp;
   if (DB.GetMetaTmp(Tmp) == false)
      return false;
   if (chdir(Tmp.c_str()) != 0)
      return _error->Errno("chdir",_("Couldn't change to %s"),Tmp.c_str());
   
   // Do extraction
   if (Tar.Go(Extract) == false)
      return false;
   
   // Switch out of the tmp directory.
   if (chdir(Cwd.c_str()) != 0)
      chdir("/");
   
   return true;
}
									/*}}}*/
// DebFile::ExtractArchive - Extract the archive data itself		/*{{{*/
// ---------------------------------------------------------------------
/* Simple wrapper around tar.. */
bool debDebFile::ExtractArchive(pkgDirStream &Stream)
{
   // Get the archive member
   const ARArchive::Member *Member = NULL;
   std::string Compressor;

   std::string const data = "data.tar";
   std::vector<APT::Configuration::Compressor> compressor = APT::Configuration::getCompressors();
   for (std::vector<APT::Configuration::Compressor>::const_iterator c = compressor.begin();
	c != compressor.end(); ++c)
   {
      Member = AR.FindMember(std::string(data).append(c->Extension).c_str());
      if (Member == NULL)
	 continue;
      Compressor = c->Binary;
      break;
   }

   if (Member == NULL)
   {
      std::string ext = "data.tar.{";
      for (std::vector<APT::Configuration::Compressor>::const_iterator c = compressor.begin();
	   c != compressor.end(); ++c)
	 ext.append(c->Extension.substr(1));
      ext.append("}");
      return _error->Error(_("Internal error, could not locate member %s"), ext.c_str());
   }

   if (File.Seek(Member->Start) == false)
      return false;

   // Prepare Tar
   ExtractTar Tar(File,Member->Size,Compressor);
   if (_error->PendingError() == true)
      return false;
   return Tar.Go(Stream);
}
									/*}}}*/
// DebFile::MergeControl - Merge the control information		/*{{{*/
// ---------------------------------------------------------------------
/* This reads the extracted control file into the cache and returns the
   version that was parsed. All this really does is select the correct
   parser and correct file to parse. */
pkgCache::VerIterator debDebFile::MergeControl(pkgDataBase &DB)
{
   // Open the control file
   std::string Tmp;
   if (DB.GetMetaTmp(Tmp) == false)
      return pkgCache::VerIterator(DB.GetCache());
   FileFd Fd(Tmp + "control",FileFd::ReadOnly);
   if (_error->PendingError() == true)
      return pkgCache::VerIterator(DB.GetCache());
   
   // Parse it
   debListParser Parse(&Fd);
   pkgCache::VerIterator Ver(DB.GetCache());
   if (DB.GetGenerator().MergeList(Parse,&Ver) == false)
      return pkgCache::VerIterator(DB.GetCache());
   
   if (Ver.end() == true)
      _error->Error(_("Failed to locate a valid control file"));
   return Ver;
}
									/*}}}*/

// DebFile::ControlExtract::DoItem - Control Tar Extraction		/*{{{*/
// ---------------------------------------------------------------------
/* This directory stream handler for the control tar handles extracting
   it into the temporary meta directory. It only extracts files, it does
   not create directories, links or anything else. */
bool debDebFile::ControlExtract::DoItem(Item &Itm,int &Fd)
{
   if (Itm.Type != Item::File)
      return true;
   
   /* Cleanse the file name, prevent people from trying to unpack into
      absolute paths, .., etc */
   for (char *I = Itm.Name; *I != 0; I++)
      if (*I == '/')
	 *I = '_';

   /* Force the ownership to be root and ensure correct permissions, 
      go-w, the rest are left untouched */
   Itm.UID = 0;
   Itm.GID = 0;
   Itm.Mode &= ~(S_IWGRP | S_IWOTH);
   
   return pkgDirStream::DoItem(Itm,Fd);
}
									/*}}}*/

// MemControlExtract::DoItem - Check if it is the control file		/*{{{*/
// ---------------------------------------------------------------------
/* This sets up to extract the control block member file into a memory 
   block of just the right size. All other files go into the bit bucket. */
bool debDebFile::MemControlExtract::DoItem(Item &Itm,int &Fd)
{
   // At the control file, allocate buffer memory.
   if (Member == Itm.Name)
   {
      delete [] Control;
      Control = new char[Itm.Size+2];
      IsControl = true;
      Fd = -2; // Signal to pass to Process
      Length = Itm.Size;
   }   
   else
      IsControl = false;
   
   return true;
}
									/*}}}*/
// MemControlExtract::Process - Process extracting the control file	/*{{{*/
// ---------------------------------------------------------------------
/* Just memcopy the block from the tar extractor and put it in the right
   place in the pre-allocated memory block. */
bool debDebFile::MemControlExtract::Process(Item &Itm,const unsigned char *Data,
			     unsigned long Size,unsigned long Pos)
{
   memcpy(Control + Pos, Data,Size);
   return true;
}
									/*}}}*/
// MemControlExtract::Read - Read the control information from the deb	/*{{{*/
// ---------------------------------------------------------------------
/* This uses the internal tar extractor to fetch the control file, and then
   it parses it into a tag section parser. */
bool debDebFile::MemControlExtract::Read(debDebFile &Deb)
{
   // Get the archive member and positition the file 
   const ARArchive::Member *Member = Deb.GotoMember("control.tar.gz");
   if (Member == 0)
      return false;

   // Extract it.
   ExtractTar Tar(Deb.GetFile(),Member->Size,"gzip");
   if (Tar.Go(*this) == false)
      return false;

   if (Control == 0)
      return true;
   
   Control[Length] = '\n';
   Control[Length+1] = '\n';
   if (Section.Scan(Control,Length+2) == false)
      return _error->Error(_("Unparsable control file"));
   return true;
}
									/*}}}*/
// MemControlExtract::TakeControl - Parse a memory block		/*{{{*/
// ---------------------------------------------------------------------
/* The given memory block is loaded into the parser and parsed as a control
   record. */
bool debDebFile::MemControlExtract::TakeControl(const void *Data,unsigned long Size)
{
   delete [] Control;
   Control = new char[Size+2];
   Length = Size;
   memcpy(Control,Data,Size);
   
   Control[Length] = '\n';
   Control[Length+1] = '\n';
   return Section.Scan(Control,Length+2);
}
									/*}}}*/

