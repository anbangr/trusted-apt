// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: writer.cc,v 1.14 2004/03/24 01:40:43 mdz Exp $
/* ######################################################################

   Writer 
   
   The file writer classes. These write various types of output, sources,
   packages and contents.
   
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include <config.h>

#include <apt-pkg/strutl.h>
#include <apt-pkg/error.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/aptconfiguration.h>
#include <apt-pkg/md5.h>
#include <apt-pkg/hashes.h>
#include <apt-pkg/deblistparser.h>

#include <sys/types.h>
#include <unistd.h>
#include <ctime>
#include <ftw.h>
#include <fnmatch.h>
#include <iostream>
#include <sstream>
#include <memory>

#include "writer.h"
#include "cachedb.h"
#include "apt-ftparchive.h"
#include "multicompress.h"

#include <apti18n.h>
									/*}}}*/
using namespace std;
FTWScanner *FTWScanner::Owner;

// SetTFRewriteData - Helper for setting rewrite lists			/*{{{*/
// ---------------------------------------------------------------------
/* */
inline void SetTFRewriteData(struct TFRewriteData &tfrd,
			     const char *tag,
			     const char *rewrite,
			     const char *newtag = 0)
{
    tfrd.Tag = tag;
    tfrd.Rewrite = rewrite;
    tfrd.NewTag = newtag;
}
									/*}}}*/

// FTWScanner::FTWScanner - Constructor					/*{{{*/
// ---------------------------------------------------------------------
/* */
FTWScanner::FTWScanner(string const &Arch): Arch(Arch)
{
   ErrorPrinted = false;
   NoLinkAct = !_config->FindB("APT::FTPArchive::DeLinkAct",true);

   DoMD5 = _config->FindB("APT::FTPArchive::MD5",true);
   DoSHA1 = _config->FindB("APT::FTPArchive::SHA1",true);
   DoSHA256 = _config->FindB("APT::FTPArchive::SHA256",true);
   DoSHA512 = _config->FindB("APT::FTPArchive::SHA512",true);
}
									/*}}}*/
// FTWScanner::Scanner - FTW Scanner					/*{{{*/
// ---------------------------------------------------------------------
/* This is the FTW scanner, it processes each directory element in the 
   directory tree. */
int FTWScanner::ScannerFTW(const char *File,const struct stat *sb,int Flag)
{
   if (Flag == FTW_DNR)
   {
      Owner->NewLine(1);
      ioprintf(c1out, _("W: Unable to read directory %s\n"), File);
   }   
   if (Flag == FTW_NS)
   {
      Owner->NewLine(1);
      ioprintf(c1out, _("W: Unable to stat %s\n"), File);
   }   
   if (Flag != FTW_F)
      return 0;

   return ScannerFile(File, true);
}
									/*}}}*/
// FTWScanner::ScannerFile - File Scanner				/*{{{*/
// ---------------------------------------------------------------------
/* */
int FTWScanner::ScannerFile(const char *File, bool const &ReadLink)
{
   const char *LastComponent = strrchr(File, '/');
   char *RealPath = NULL;

   if (LastComponent == NULL)
      LastComponent = File;
   else
      LastComponent++;

   vector<string>::const_iterator I;
   for(I = Owner->Patterns.begin(); I != Owner->Patterns.end(); ++I)
   {
      if (fnmatch((*I).c_str(), LastComponent, 0) == 0)
         break;
   }
   if (I == Owner->Patterns.end())
      return 0;

   /* Process it. If the file is a link then resolve it into an absolute
      name.. This works best if the directory components the scanner are
      given are not links themselves. */
   char Jnk[2];
   Owner->OriginalPath = File;
   if (ReadLink &&
       readlink(File,Jnk,sizeof(Jnk)) != -1 &&
       (RealPath = realpath(File,NULL)) != 0)
   {
      Owner->DoPackage(RealPath);
      free(RealPath);
   }
   else
      Owner->DoPackage(File);
   
   if (_error->empty() == false)
   {
      // Print any errors or warnings found
      string Err;
      bool SeenPath = false;
      while (_error->empty() == false)
      {
	 Owner->NewLine(1);
	 
	 bool const Type = _error->PopMessage(Err);
	 if (Type == true)
	    cerr << _("E: ") << Err << endl;
	 else
	    cerr << _("W: ") << Err << endl;
	 
	 if (Err.find(File) != string::npos)
	    SeenPath = true;
      }      
      
      if (SeenPath == false)
	 cerr << _("E: Errors apply to file ") << "'" << File << "'" << endl;
      return 0;
   }
   
   return 0;
}
									/*}}}*/
// FTWScanner::RecursiveScan - Just scan a directory tree		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool FTWScanner::RecursiveScan(string const &Dir)
{
   char *RealPath = NULL;
   /* If noprefix is set then jam the scan root in, so we don't generate
      link followed paths out of control */
   if (InternalPrefix.empty() == true)
   {
      if ((RealPath = realpath(Dir.c_str(),NULL)) == 0)
	 return _error->Errno("realpath",_("Failed to resolve %s"),Dir.c_str());
      InternalPrefix = RealPath;
      free(RealPath);
   }
   
   // Do recursive directory searching
   Owner = this;
   int const Res = ftw(Dir.c_str(),ScannerFTW,30);
   
   // Error treewalking?
   if (Res != 0)
   {
      if (_error->PendingError() == false)
	 _error->Errno("ftw",_("Tree walking failed"));
      return false;
   }
   
   return true;
}
									/*}}}*/
// FTWScanner::LoadFileList - Load the file list from a file		/*{{{*/
// ---------------------------------------------------------------------
/* This is an alternative to using FTW to locate files, it reads the list
   of files from another file. */
bool FTWScanner::LoadFileList(string const &Dir, string const &File)
{
   char *RealPath = NULL;
   /* If noprefix is set then jam the scan root in, so we don't generate
      link followed paths out of control */
   if (InternalPrefix.empty() == true)
   {
      if ((RealPath = realpath(Dir.c_str(),NULL)) == 0)
	 return _error->Errno("realpath",_("Failed to resolve %s"),Dir.c_str());
      InternalPrefix = RealPath;      
      free(RealPath);
   }
   
   Owner = this;
   FILE *List = fopen(File.c_str(),"r");
   if (List == 0)
      return _error->Errno("fopen",_("Failed to open %s"),File.c_str());
   
   /* We are a tad tricky here.. We prefix the buffer with the directory
      name, that way if we need a full path with just use line.. Sneaky and
      fully evil. */
   char Line[1000];
   char *FileStart;
   if (Dir.empty() == true || Dir.end()[-1] != '/')
      FileStart = Line + snprintf(Line,sizeof(Line),"%s/",Dir.c_str());
   else
      FileStart = Line + snprintf(Line,sizeof(Line),"%s",Dir.c_str());   
   while (fgets(FileStart,sizeof(Line) - (FileStart - Line),List) != 0)
   {
      char *FileName = _strstrip(FileStart);
      if (FileName[0] == 0)
	 continue;
	 
      if (FileName[0] != '/')
      {
	 if (FileName != FileStart)
	    memmove(FileStart,FileName,strlen(FileStart));
	 FileName = Line;
      }
      
#if 0
      struct stat St;
      int Flag = FTW_F;
      if (stat(FileName,&St) != 0)
	 Flag = FTW_NS;
#endif

      if (ScannerFile(FileName, false) != 0)
	 break;
   }
  
   fclose(List);
   return true;
}
									/*}}}*/
// FTWScanner::Delink - Delink symlinks					/*{{{*/
// ---------------------------------------------------------------------
/* */
bool FTWScanner::Delink(string &FileName,const char *OriginalPath,
			unsigned long long &DeLinkBytes,
			unsigned long long const &FileSize)
{
   // See if this isn't an internaly prefix'd file name.
   if (InternalPrefix.empty() == false &&
       InternalPrefix.length() < FileName.length() && 
       stringcmp(FileName.begin(),FileName.begin() + InternalPrefix.length(),
		 InternalPrefix.begin(),InternalPrefix.end()) != 0)
   {
      if (DeLinkLimit != 0 && DeLinkBytes/1024 < DeLinkLimit)
      {
	 // Tidy up the display
	 if (DeLinkBytes == 0)
	    cout << endl;
	 
	 NewLine(1);
	 ioprintf(c1out, _(" DeLink %s [%s]\n"), (OriginalPath + InternalPrefix.length()),
		    SizeToStr(FileSize).c_str());
	 c1out << flush;
	 
	 if (NoLinkAct == false)
	 {
	    char OldLink[400];
	    if (readlink(OriginalPath,OldLink,sizeof(OldLink)) == -1)
	       _error->Errno("readlink",_("Failed to readlink %s"),OriginalPath);
	    else
	    {
	       if (unlink(OriginalPath) != 0)
		  _error->Errno("unlink",_("Failed to unlink %s"),OriginalPath);
	       else
	       {
		  if (link(FileName.c_str(),OriginalPath) != 0)
		  {
		     // Panic! Restore the symlink
		     symlink(OldLink,OriginalPath);
		     return _error->Errno("link",_("*** Failed to link %s to %s"),
					  FileName.c_str(),
					  OriginalPath);
		  }	       
	       }
	    }	    
	 }
	 
	 DeLinkBytes += FileSize;
	 if (DeLinkBytes/1024 >= DeLinkLimit)
	    ioprintf(c1out, _(" DeLink limit of %sB hit.\n"), SizeToStr(DeLinkBytes).c_str());      
      }
      
      FileName = OriginalPath;
   }
   
   return true;
}
									/*}}}*/

// PackagesWriter::PackagesWriter - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* */
PackagesWriter::PackagesWriter(string const &DB,string const &Overrides,string const &ExtOverrides,
			       string const &Arch) :
   FTWScanner(Arch), Db(DB), Stats(Db.Stats), TransWriter(NULL)
{
   Output = stdout;
   SetExts(".deb .udeb");
   DeLinkLimit = 0;

   // Process the command line options
   DoMD5 = _config->FindB("APT::FTPArchive::Packages::MD5",DoMD5);
   DoSHA1 = _config->FindB("APT::FTPArchive::Packages::SHA1",DoSHA1);
   DoSHA256 = _config->FindB("APT::FTPArchive::Packages::SHA256",DoSHA256);
   DoSHA256 = _config->FindB("APT::FTPArchive::Packages::SHA512",true);
   DoAlwaysStat = _config->FindB("APT::FTPArchive::AlwaysStat", false);
   DoContents = _config->FindB("APT::FTPArchive::Contents",true);
   NoOverride = _config->FindB("APT::FTPArchive::NoOverrideMsg",false);
   LongDescription = _config->FindB("APT::FTPArchive::LongDescription",true);

   if (Db.Loaded() == false)
      DoContents = false;

   // Read the override file
   if (Overrides.empty() == false && Over.ReadOverride(Overrides) == false)
      return;
   else
      NoOverride = true;

   if (ExtOverrides.empty() == false)
      Over.ReadExtraOverride(ExtOverrides);

   _error->DumpErrors();
}
                                                                        /*}}}*/
// FTWScanner::SetExts - Set extensions to support                      /*{{{*/
// ---------------------------------------------------------------------
/* */
bool FTWScanner::SetExts(string const &Vals)
{
   ClearPatterns();
   string::size_type Start = 0;
   while (Start <= Vals.length()-1)
   {
      string::size_type const Space = Vals.find(' ',Start);
      string::size_type const Length = ((Space == string::npos) ? Vals.length() : Space) - Start;
      if ( Arch.empty() == false )
      {
	 AddPattern(string("*_") + Arch + Vals.substr(Start, Length));
	 AddPattern(string("*_all") + Vals.substr(Start, Length));
      }
      else
	 AddPattern(string("*") + Vals.substr(Start, Length));

      Start += Length + 1;
   }

   return true;
}

									/*}}}*/
// PackagesWriter::DoPackage - Process a single package			/*{{{*/
// ---------------------------------------------------------------------
/* This method takes a package and gets its control information and 
   MD5, SHA1 and SHA256 then writes out a control record with the proper fields 
   rewritten and the path/size/hash appended. */
bool PackagesWriter::DoPackage(string FileName)
{      
   // Pull all the data we need form the DB
   if (Db.GetFileInfo(FileName, true, DoContents, true, DoMD5, DoSHA1, DoSHA256, DoSHA512, DoAlwaysStat)
		  == false)
   {
      return false;
   }

   unsigned long long FileSize = Db.GetFileSize();
   if (Delink(FileName,OriginalPath,Stats.DeLinkBytes,FileSize) == false)
      return false;
   
   // Lookup the overide information
   pkgTagSection &Tags = Db.Control.Section;
   string Package = Tags.FindS("Package");
   string Architecture;
   // if we generate a Packages file for a given arch, we use it to
   // look for overrides. if we run in "simple" mode without the 
   // "Architecures" variable in the config we use the architecure value
   // from the deb file
   if(Arch != "")
      Architecture = Arch;
   else
      Architecture = Tags.FindS("Architecture");
   auto_ptr<Override::Item> OverItem(Over.GetItem(Package,Architecture));
   
   if (Package.empty() == true)
      return _error->Error(_("Archive had no package field"));

   // If we need to do any rewriting of the header do it now..
   if (OverItem.get() == 0)
   {
      if (NoOverride == false)
      {
	 NewLine(1);
	 ioprintf(c1out, _("  %s has no override entry\n"), Package.c_str());
      }
      
      OverItem = auto_ptr<Override::Item>(new Override::Item);
      OverItem->FieldOverride["Section"] = Tags.FindS("Section");
      OverItem->Priority = Tags.FindS("Priority");
   }

   char Size[40];
   sprintf(Size,"%llu", (unsigned long long) FileSize);
   
   // Strip the DirStrip prefix from the FileName and add the PathPrefix
   string NewFileName;
   if (DirStrip.empty() == false &&
       FileName.length() > DirStrip.length() &&
       stringcmp(FileName.begin(),FileName.begin() + DirStrip.length(),
		 DirStrip.begin(),DirStrip.end()) == 0)
      NewFileName = string(FileName.begin() + DirStrip.length(),FileName.end());
   else 
      NewFileName = FileName;
   if (PathPrefix.empty() == false)
      NewFileName = flCombine(PathPrefix,NewFileName);

   /* Configuration says we don't want to include the long Description
      in the package file - instead we want to ship a separated file */
   string desc;
   if (LongDescription == false) {
      desc = Tags.FindS("Description").append("\n");
      OverItem->FieldOverride["Description"] = desc.substr(0, desc.find('\n')).c_str();
   }

   // This lists all the changes to the fields we are going to make.
   // (7 hardcoded + maintainer + suggests + end marker)
   TFRewriteData Changes[6+2+OverItem->FieldOverride.size()+1+1];

   unsigned int End = 0;
   SetTFRewriteData(Changes[End++], "Size", Size);
   if (DoMD5 == true)
      SetTFRewriteData(Changes[End++], "MD5sum", Db.MD5Res.c_str());
   if (DoSHA1 == true)
      SetTFRewriteData(Changes[End++], "SHA1", Db.SHA1Res.c_str());
   if (DoSHA256 == true)
      SetTFRewriteData(Changes[End++], "SHA256", Db.SHA256Res.c_str());
   if (DoSHA512 == true)
      SetTFRewriteData(Changes[End++], "SHA512", Db.SHA512Res.c_str());
   SetTFRewriteData(Changes[End++], "Filename", NewFileName.c_str());
   SetTFRewriteData(Changes[End++], "Priority", OverItem->Priority.c_str());
   SetTFRewriteData(Changes[End++], "Status", 0);
   SetTFRewriteData(Changes[End++], "Optional", 0);

   string DescriptionMd5;
   if (LongDescription == false) {
      MD5Summation descmd5;
      descmd5.Add(desc.c_str());
      DescriptionMd5 = descmd5.Result().Value();
      SetTFRewriteData(Changes[End++], "Description-md5", DescriptionMd5.c_str());
      if (TransWriter != NULL)
	 TransWriter->DoPackage(Package, desc, DescriptionMd5);
   }

   // Rewrite the maintainer field if necessary
   bool MaintFailed;
   string NewMaint = OverItem->SwapMaint(Tags.FindS("Maintainer"),MaintFailed);
   if (MaintFailed == true)
   {
      if (NoOverride == false)
      {
	 NewLine(1);
	 ioprintf(c1out, _("  %s maintainer is %s not %s\n"),
	       Package.c_str(), Tags.FindS("Maintainer").c_str(), OverItem->OldMaint.c_str());
      }      
   }
   
   if (NewMaint.empty() == false)
      SetTFRewriteData(Changes[End++], "Maintainer", NewMaint.c_str());
   
   /* Get rid of the Optional tag. This is an ugly, ugly, ugly hack that
      dpkg-scanpackages does. Well sort of. dpkg-scanpackages just does renaming
      but dpkg does this append bit. So we do the append bit, at least that way the
      status file and package file will remain similar. There are other transforms
      but optional is the only legacy one still in use for some lazy reason. */
   string OptionalStr = Tags.FindS("Optional");
   if (OptionalStr.empty() == false)
   {
      if (Tags.FindS("Suggests").empty() == false)
	 OptionalStr = Tags.FindS("Suggests") + ", " + OptionalStr;
      SetTFRewriteData(Changes[End++], "Suggests", OptionalStr.c_str());
   }

   for (map<string,string>::const_iterator I = OverItem->FieldOverride.begin(); 
        I != OverItem->FieldOverride.end(); ++I)
      SetTFRewriteData(Changes[End++],I->first.c_str(),I->second.c_str());

   SetTFRewriteData(Changes[End++], 0, 0);

   // Rewrite and store the fields.
   if (TFRewrite(Output,Tags,TFRewritePackageOrder,Changes) == false)
      return false;
   fprintf(Output,"\n");

   return Db.Finish();
}
									/*}}}*/

// TranslationWriter::TranslationWriter - Constructor			/*{{{*/
// ---------------------------------------------------------------------
/* Create a Translation-Master file for this Packages file */
TranslationWriter::TranslationWriter(string const &File, string const &TransCompress,
					mode_t const &Permissions) : Output(NULL),
							RefCounter(0)
{
   if (File.empty() == true)
      return;

   Comp = new MultiCompress(File, TransCompress, Permissions);
   Output = Comp->Input;
}
									/*}}}*/
// TranslationWriter::DoPackage - Process a single package		/*{{{*/
// ---------------------------------------------------------------------
/* Create a Translation-Master file for this Packages file */
bool TranslationWriter::DoPackage(string const &Pkg, string const &Desc,
				  string const &MD5)
{
   if (Output == NULL)
      return true;

   // Different archs can include different versions and therefore
   // different descriptions - so we need to check for both name and md5.
   string const Record = Pkg + ":" + MD5;

   if (Included.find(Record) != Included.end())
      return true;

   fprintf(Output, "Package: %s\nDescription-md5: %s\nDescription-en: %s\n",
	   Pkg.c_str(), MD5.c_str(), Desc.c_str());

   Included.insert(Record);
   return true;
}
									/*}}}*/
// TranslationWriter::~TranslationWriter - Destructor			/*{{{*/
// ---------------------------------------------------------------------
/* */
TranslationWriter::~TranslationWriter()
{
   if (Comp == NULL)
      return;

   delete Comp;
}
									/*}}}*/

// SourcesWriter::SourcesWriter - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* */
SourcesWriter::SourcesWriter(string const &BOverrides,string const &SOverrides,
			     string const &ExtOverrides)
{
   Output = stdout;
   AddPattern("*.dsc");
   DeLinkLimit = 0;
   Buffer = 0;
   BufSize = 0;
   
   // Process the command line options
   DoMD5 = _config->FindB("APT::FTPArchive::Sources::MD5",DoMD5);
   DoSHA1 = _config->FindB("APT::FTPArchive::Sources::SHA1",DoSHA1);
   DoSHA256 = _config->FindB("APT::FTPArchive::Sources::SHA256",DoSHA256);
   NoOverride = _config->FindB("APT::FTPArchive::NoOverrideMsg",false);

   // Read the override file
   if (BOverrides.empty() == false && BOver.ReadOverride(BOverrides) == false)
      return;
   else
      NoOverride = true;

   // WTF?? The logic above: if we can't read binary overrides, don't even try
   // reading source overrides. if we can read binary overrides, then say there
   // are no overrides. THIS MAKES NO SENSE! -- ajt@d.o, 2006/02/28

   if (ExtOverrides.empty() == false)
      SOver.ReadExtraOverride(ExtOverrides);
   
   if (SOverrides.empty() == false && FileExists(SOverrides) == true)
      SOver.ReadOverride(SOverrides,true);
}
									/*}}}*/
// SourcesWriter::DoPackage - Process a single package			/*{{{*/
// ---------------------------------------------------------------------
/* */
bool SourcesWriter::DoPackage(string FileName)
{      
   // Open the archive
   FileFd F(FileName,FileFd::ReadOnly);
   if (_error->PendingError() == true)
      return false;
   
   // Stat the file for later
   struct stat St;
   if (fstat(F.Fd(),&St) != 0)
      return _error->Errno("fstat","Failed to stat %s",FileName.c_str());

   if (St.st_size > 128*1024)
      return _error->Error("DSC file '%s' is too large!",FileName.c_str());
         
   if (BufSize < (unsigned long long)St.st_size+1)
   {
      BufSize = St.st_size+1;
      Buffer = (char *)realloc(Buffer,St.st_size+1);
   }
   
   if (F.Read(Buffer,St.st_size) == false)
      return false;

   // Hash the file
   char *Start = Buffer;
   char *BlkEnd = Buffer + St.st_size;

   MD5Summation MD5;
   SHA1Summation SHA1;
   SHA256Summation SHA256;
   SHA256Summation SHA512;

   if (DoMD5 == true)
      MD5.Add((unsigned char *)Start,BlkEnd - Start);
   if (DoSHA1 == true)
      SHA1.Add((unsigned char *)Start,BlkEnd - Start);
   if (DoSHA256 == true)
      SHA256.Add((unsigned char *)Start,BlkEnd - Start);
   if (DoSHA512 == true)
      SHA512.Add((unsigned char *)Start,BlkEnd - Start);

   // Add an extra \n to the end, just in case
   *BlkEnd++ = '\n';
   
   /* Remove the PGP trailer. Some .dsc's have this without a blank line 
      before */
   const char *Key = "-----BEGIN PGP SIGNATURE-----";
   for (char *MsgEnd = Start; MsgEnd < BlkEnd - strlen(Key) -1; MsgEnd++)
   {
      if (*MsgEnd == '\n' && strncmp(MsgEnd+1,Key,strlen(Key)) == 0)
      {
	 MsgEnd[1] = '\n';
	 break;
      }      
   }
   
   /* Read records until we locate the Source record. This neatly skips the
      GPG header (which is RFC822 formed) without any trouble. */
   pkgTagSection Tags;
   do
   {
      unsigned Pos;
      if (Tags.Scan(Start,BlkEnd - Start) == false)
	 return _error->Error("Could not find a record in the DSC '%s'",FileName.c_str());
      if (Tags.Find("Source",Pos) == true)
	 break;
      Start += Tags.size();
   }
   while (1);
   Tags.Trim();
      
   // Lookup the overide information, finding first the best priority.
   string BestPrio;
   string Bins = Tags.FindS("Binary");
   char Buffer[Bins.length() + 1];
   auto_ptr<Override::Item> OverItem(0);
   if (Bins.empty() == false)
   {
      strcpy(Buffer,Bins.c_str());
      
      // Ignore too-long errors.
      char *BinList[400];
      TokSplitString(',',Buffer,BinList,sizeof(BinList)/sizeof(BinList[0]));
      
      // Look at all the binaries
      unsigned char BestPrioV = pkgCache::State::Extra;
      for (unsigned I = 0; BinList[I] != 0; I++)
      {
	 auto_ptr<Override::Item> Itm(BOver.GetItem(BinList[I]));
	 if (Itm.get() == 0)
	    continue;

	 unsigned char NewPrioV = debListParser::GetPrio(Itm->Priority);
	 if (NewPrioV < BestPrioV || BestPrio.empty() == true)
	 {
	    BestPrioV = NewPrioV;
	    BestPrio = Itm->Priority;
	 }	 

	 if (OverItem.get() == 0)
	    OverItem = Itm;
      }
   }
   
   // If we need to do any rewriting of the header do it now..
   if (OverItem.get() == 0)
   {
      if (NoOverride == false)
      {
	 NewLine(1);	 
	 ioprintf(c1out, _("  %s has no override entry\n"), Tags.FindS("Source").c_str());
      }
      
      OverItem = auto_ptr<Override::Item>(new Override::Item);
   }
   
   auto_ptr<Override::Item> SOverItem(SOver.GetItem(Tags.FindS("Source")));
   // const auto_ptr<Override::Item> autoSOverItem(SOverItem);
   if (SOverItem.get() == 0)
   {
      ioprintf(c1out, _("  %s has no source override entry\n"), Tags.FindS("Source").c_str());
      SOverItem = auto_ptr<Override::Item>(BOver.GetItem(Tags.FindS("Source")));
      if (SOverItem.get() == 0)
      {
        ioprintf(c1out, _("  %s has no binary override entry either\n"), Tags.FindS("Source").c_str());
	 SOverItem = auto_ptr<Override::Item>(new Override::Item);
	 *SOverItem = *OverItem;
      }
   }
   
   // Add the dsc to the files hash list
   string const strippedName = flNotDir(FileName);
   std::ostringstream ostreamFiles;
   if (DoMD5 == true && Tags.Exists("Files"))
      ostreamFiles << "\n " << string(MD5.Result()) << " " << St.st_size << " "
		   << strippedName << "\n " << Tags.FindS("Files");
   string const Files = ostreamFiles.str();

   std::ostringstream ostreamSha1;
   if (DoSHA1 == true && Tags.Exists("Checksums-Sha1"))
      ostreamSha1 << "\n " << string(SHA1.Result()) << " " << St.st_size << " "
		   << strippedName << "\n " << Tags.FindS("Checksums-Sha1");
   string const ChecksumsSha1 = ostreamSha1.str();

   std::ostringstream ostreamSha256;
   if (DoSHA256 == true && Tags.Exists("Checksums-Sha256"))
      ostreamSha256 << "\n " << string(SHA256.Result()) << " " << St.st_size << " "
		   << strippedName << "\n " << Tags.FindS("Checksums-Sha256");
   string const ChecksumsSha256 = ostreamSha256.str();

   std::ostringstream ostreamSha512;
   if (Tags.Exists("Checksums-Sha512"))
      ostreamSha512 << "\n " << string(SHA512.Result()) << " " << St.st_size << " "
		   << strippedName << "\n " << Tags.FindS("Checksums-Sha512");
   string const ChecksumsSha512 = ostreamSha512.str();

   // Strip the DirStrip prefix from the FileName and add the PathPrefix
   string NewFileName;
   if (DirStrip.empty() == false &&
       FileName.length() > DirStrip.length() &&
       stringcmp(DirStrip,OriginalPath,OriginalPath + DirStrip.length()) == 0)
      NewFileName = string(OriginalPath + DirStrip.length());
   else 
      NewFileName = OriginalPath;
   if (PathPrefix.empty() == false)
      NewFileName = flCombine(PathPrefix,NewFileName);

   string Directory = flNotFile(OriginalPath);
   string Package = Tags.FindS("Source");

   // Perform the delinking operation over all of the files
   string ParseJnk;
   const char *C = Files.c_str();
   char *RealPath = NULL;
   for (;isspace(*C); C++);
   while (*C != 0)
   {   
      // Parse each of the elements
      if (ParseQuoteWord(C,ParseJnk) == false ||
	  ParseQuoteWord(C,ParseJnk) == false ||
	  ParseQuoteWord(C,ParseJnk) == false)
	 return _error->Error("Error parsing file record");
      
      char Jnk[2];
      string OriginalPath = Directory + ParseJnk;
      if (readlink(OriginalPath.c_str(),Jnk,sizeof(Jnk)) != -1 &&
	  (RealPath = realpath(OriginalPath.c_str(),NULL)) != 0)
      {
	 string RP = RealPath;
	 free(RealPath);
	 if (Delink(RP,OriginalPath.c_str(),Stats.DeLinkBytes,St.st_size) == false)
	    return false;
      }
   }

   Directory = flNotFile(NewFileName);
   if (Directory.length() > 2)
      Directory.erase(Directory.end()-1);

   // This lists all the changes to the fields we are going to make.
   // (5 hardcoded + checksums + maintainer + end marker)
   TFRewriteData Changes[5+2+1+SOverItem->FieldOverride.size()+1];

   unsigned int End = 0;
   SetTFRewriteData(Changes[End++],"Source",Package.c_str(),"Package");
   if (Files.empty() == false)
      SetTFRewriteData(Changes[End++],"Files",Files.c_str());
   if (ChecksumsSha1.empty() == false)
      SetTFRewriteData(Changes[End++],"Checksums-Sha1",ChecksumsSha1.c_str());
   if (ChecksumsSha256.empty() == false)
      SetTFRewriteData(Changes[End++],"Checksums-Sha256",ChecksumsSha256.c_str());
   if (ChecksumsSha512.empty() == false)
      SetTFRewriteData(Changes[End++],"Checksums-Sha512",ChecksumsSha512.c_str());
   if (Directory != "./")
      SetTFRewriteData(Changes[End++],"Directory",Directory.c_str());
   SetTFRewriteData(Changes[End++],"Priority",BestPrio.c_str());
   SetTFRewriteData(Changes[End++],"Status",0);

   // Rewrite the maintainer field if necessary
   bool MaintFailed;
   string NewMaint = OverItem->SwapMaint(Tags.FindS("Maintainer"),MaintFailed);
   if (MaintFailed == true)
   {
      if (NoOverride == false)
      {
	 NewLine(1);	 
	 ioprintf(c1out, _("  %s maintainer is %s not %s\n"), Package.c_str(),
	       Tags.FindS("Maintainer").c_str(), OverItem->OldMaint.c_str());
      }      
   }
   if (NewMaint.empty() == false)
      SetTFRewriteData(Changes[End++], "Maintainer", NewMaint.c_str());
   
   for (map<string,string>::const_iterator I = SOverItem->FieldOverride.begin(); 
        I != SOverItem->FieldOverride.end(); ++I)
      SetTFRewriteData(Changes[End++],I->first.c_str(),I->second.c_str());

   SetTFRewriteData(Changes[End++], 0, 0);
      
   // Rewrite and store the fields.
   if (TFRewrite(Output,Tags,TFRewriteSourceOrder,Changes) == false)
      return false;
   fprintf(Output,"\n");

   Stats.Packages++;
   
   return true;
}
									/*}}}*/

// ContentsWriter::ContentsWriter - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* */
ContentsWriter::ContentsWriter(string const &DB, string const &Arch) :
		    FTWScanner(Arch), Db(DB), Stats(Db.Stats)

{
   SetExts(".deb");
   Output = stdout;
}
									/*}}}*/
// ContentsWriter::DoPackage - Process a single package			/*{{{*/
// ---------------------------------------------------------------------
/* If Package is the empty string the control record will be parsed to
   determine what the package name is. */
bool ContentsWriter::DoPackage(string FileName, string Package)
{
   if (!Db.GetFileInfo(FileName, Package.empty(), true, false, false, false, false, false))
   {
      return false;
   }

   // Parse the package name
   if (Package.empty() == true)
   {
      Package = Db.Control.Section.FindS("Package");
   }

   Db.Contents.Add(Gen,Package);
   
   return Db.Finish();
}
									/*}}}*/
// ContentsWriter::ReadFromPkgs - Read from a packages file		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool ContentsWriter::ReadFromPkgs(string const &PkgFile,string const &PkgCompress)
{
   MultiCompress Pkgs(PkgFile,PkgCompress,0,false);
   if (_error->PendingError() == true)
      return false;

   // Open the package file
   FileFd Fd;
   if (Pkgs.OpenOld(Fd) == false)
      return false;

   pkgTagFile Tags(&Fd);
   if (_error->PendingError() == true)
      return false;

   // Parse.
   pkgTagSection Section;
   while (Tags.Step(Section) == true)
   {
      string File = flCombine(Prefix,Section.FindS("FileName"));
      string Package = Section.FindS("Section");
      if (Package.empty() == false && Package.end()[-1] != '/')
      {
	 Package += '/';
	 Package += Section.FindS("Package");
      }
      else
	 Package += Section.FindS("Package");
	 
      DoPackage(File,Package);
      if (_error->empty() == false)
      {
	 _error->Error("Errors apply to file '%s'",File.c_str());
	 _error->DumpErrors();
      }
   }

   // Tidy the compressor
   Fd.Close();

   return true;
}

									/*}}}*/

// ReleaseWriter::ReleaseWriter - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* */
ReleaseWriter::ReleaseWriter(string const &DB)
{
   if (_config->FindB("APT::FTPArchive::Release::Default-Patterns", true) == true)
   {
      AddPattern("Packages");
      AddPattern("Packages.gz");
      AddPattern("Packages.bz2");
      AddPattern("Packages.lzma");
      AddPattern("Packages.xz");
      AddPattern("Sources");
      AddPattern("Sources.gz");
      AddPattern("Sources.bz2");
      AddPattern("Sources.lzma");
      AddPattern("Sources.xz");
      AddPattern("Release");
      AddPattern("Index");
      AddPattern("md5sum.txt");
   }
   AddPatterns(_config->FindVector("APT::FTPArchive::Release::Patterns"));

   Output = stdout;
   time_t const now = time(NULL);

   setlocale(LC_TIME, "C");

   char datestr[128];
   if (strftime(datestr, sizeof(datestr), "%a, %d %b %Y %H:%M:%S UTC",
                gmtime(&now)) == 0)
   {
      datestr[0] = '\0';
   }

   time_t const validuntil = now + _config->FindI("APT::FTPArchive::Release::ValidTime", 0);
   char validstr[128];
   if (now == validuntil ||
       strftime(validstr, sizeof(validstr), "%a, %d %b %Y %H:%M:%S UTC",
                gmtime(&validuntil)) == 0)
   {
      validstr[0] = '\0';
   }

   setlocale(LC_TIME, "");

   map<string,string> Fields;
   Fields["Origin"] = "";
   Fields["Label"] = "";
   Fields["Suite"] = "";
   Fields["Version"] = "";
   Fields["Codename"] = "";
   Fields["Date"] = datestr;
   Fields["Valid-Until"] = validstr;
   Fields["Architectures"] = "";
   Fields["Components"] = "";
   Fields["Description"] = "";

   for(map<string,string>::const_iterator I = Fields.begin();
       I != Fields.end();
       ++I)
   {
      string Config = string("APT::FTPArchive::Release::") + (*I).first;
      string Value = _config->Find(Config, (*I).second.c_str());
      if (Value == "")
         continue;

      fprintf(Output, "%s: %s\n", (*I).first.c_str(), Value.c_str());
   }

   DoMD5 = _config->FindB("APT::FTPArchive::Release::MD5",DoMD5);
   DoSHA1 = _config->FindB("APT::FTPArchive::Release::SHA1",DoSHA1);
   DoSHA256 = _config->FindB("APT::FTPArchive::Release::SHA256",DoSHA256);
}
									/*}}}*/
// ReleaseWriter::DoPackage - Process a single package			/*{{{*/
// ---------------------------------------------------------------------
bool ReleaseWriter::DoPackage(string FileName)
{
   // Strip the DirStrip prefix from the FileName and add the PathPrefix
   string NewFileName;
   if (DirStrip.empty() == false &&
       FileName.length() > DirStrip.length() &&
       stringcmp(FileName.begin(),FileName.begin() + DirStrip.length(),
		 DirStrip.begin(),DirStrip.end()) == 0)
   {
      NewFileName = string(FileName.begin() + DirStrip.length(),FileName.end());
      while (NewFileName[0] == '/')
         NewFileName = string(NewFileName.begin() + 1,NewFileName.end());
   }
   else 
      NewFileName = FileName;

   if (PathPrefix.empty() == false)
      NewFileName = flCombine(PathPrefix,NewFileName);

   FileFd fd(FileName, FileFd::ReadOnly);

   if (!fd.IsOpen())
   {
      return false;
   }

   CheckSums[NewFileName].size = fd.Size();

   Hashes hs;
   hs.AddFD(fd, 0, DoMD5, DoSHA1, DoSHA256, DoSHA512);
   if (DoMD5 == true)
      CheckSums[NewFileName].MD5 = hs.MD5.Result();
   if (DoSHA1 == true)
      CheckSums[NewFileName].SHA1 = hs.SHA1.Result();
   if (DoSHA256 == true)
      CheckSums[NewFileName].SHA256 = hs.SHA256.Result();
   if (DoSHA512 == true)
      CheckSums[NewFileName].SHA512 = hs.SHA512.Result();
   fd.Close();

   return true;
}

									/*}}}*/
// ReleaseWriter::Finish - Output the checksums				/*{{{*/
// ---------------------------------------------------------------------
void ReleaseWriter::Finish()
{
   if (DoMD5 == true)
   {
      fprintf(Output, "MD5Sum:\n");
      for(map<string,struct CheckSum>::const_iterator I = CheckSums.begin();
	  I != CheckSums.end(); ++I)
      {
	 fprintf(Output, " %s %16llu %s\n",
		 (*I).second.MD5.c_str(),
		 (*I).second.size,
		 (*I).first.c_str());
      }
   }
   if (DoSHA1 == true)
   {
      fprintf(Output, "SHA1:\n");
      for(map<string,struct CheckSum>::const_iterator I = CheckSums.begin();
	  I != CheckSums.end(); ++I)
      {
	 fprintf(Output, " %s %16llu %s\n",
		 (*I).second.SHA1.c_str(),
		 (*I).second.size,
		 (*I).first.c_str());
      }
   }
   if (DoSHA256 == true)
   {
      fprintf(Output, "SHA256:\n");
      for(map<string,struct CheckSum>::const_iterator I = CheckSums.begin();
	  I != CheckSums.end(); ++I)
      {
	 fprintf(Output, " %s %16llu %s\n",
		 (*I).second.SHA256.c_str(),
		 (*I).second.size,
		 (*I).first.c_str());
      }
   }

   fprintf(Output, "SHA512:\n");
   for(map<string,struct CheckSum>::const_iterator I = CheckSums.begin();
       I != CheckSums.end();
       ++I)
   {
      fprintf(Output, " %s %16llu %s\n",
              (*I).second.SHA512.c_str(),
              (*I).second.size,
              (*I).first.c_str());
   }

}
