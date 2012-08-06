// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: debindexfile.cc,v 1.5.2.3 2004/01/04 19:11:00 mdz Exp $
/* ######################################################################

   Debian Specific sources.list types and the three sorts of Debian
   index files.
   
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include <config.h>

#include <apt-pkg/debindexfile.h>
#include <apt-pkg/debsrcrecords.h>
#include <apt-pkg/deblistparser.h>
#include <apt-pkg/debrecords.h>
#include <apt-pkg/sourcelist.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/progress.h>
#include <apt-pkg/error.h>
#include <apt-pkg/strutl.h>
#include <apt-pkg/acquire-item.h>
#include <apt-pkg/debmetaindex.h>

#include <sys/stat.h>
									/*}}}*/

using std::string;

// SourcesIndex::debSourcesIndex - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* */
debSourcesIndex::debSourcesIndex(string URI,string Dist,string Section,bool Trusted) :
     pkgIndexFile(Trusted), URI(URI), Dist(Dist), Section(Section)
{
}
									/*}}}*/
// SourcesIndex::SourceInfo - Short 1 liner describing a source		/*{{{*/
// ---------------------------------------------------------------------
/* The result looks like:
     http://foo/debian/ stable/main src 1.1.1 (dsc) */
string debSourcesIndex::SourceInfo(pkgSrcRecords::Parser const &Record,
				   pkgSrcRecords::File const &File) const
{
   string Res;
   Res = ::URI::NoUserPassword(URI) + ' ';
   if (Dist[Dist.size() - 1] == '/')
   {
      if (Dist != "/")
	 Res += Dist;
   }      
   else
      Res += Dist + '/' + Section;
   
   Res += " ";
   Res += Record.Package();
   Res += " ";
   Res += Record.Version();
   if (File.Type.empty() == false)
      Res += " (" + File.Type + ")";
   return Res;
}
									/*}}}*/
// SourcesIndex::CreateSrcParser - Get a parser for the source files	/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgSrcRecords::Parser *debSourcesIndex::CreateSrcParser() const
{
   string SourcesURI = _config->FindDir("Dir::State::lists") + 
      URItoFileName(IndexURI("Sources"));
   string SourcesURIgzip = SourcesURI + ".gz";

   if (!FileExists(SourcesURI) && !FileExists(SourcesURIgzip))
      return NULL;
   else if (!FileExists(SourcesURI) && FileExists(SourcesURIgzip))
      SourcesURI = SourcesURIgzip;

   return new debSrcRecordParser(SourcesURI,this);
}
									/*}}}*/
// SourcesIndex::Describe - Give a descriptive path to the index	/*{{{*/
// ---------------------------------------------------------------------
/* */
string debSourcesIndex::Describe(bool Short) const
{
   char S[300];
   if (Short == true)
      snprintf(S,sizeof(S),"%s",Info("Sources").c_str());
   else
      snprintf(S,sizeof(S),"%s (%s)",Info("Sources").c_str(),
	       IndexFile("Sources").c_str());
   
   return S;
}
									/*}}}*/
// SourcesIndex::Info - One liner describing the index URI		/*{{{*/
// ---------------------------------------------------------------------
/* */
string debSourcesIndex::Info(const char *Type) const
{
   string Info = ::URI::NoUserPassword(URI) + ' ';
   if (Dist[Dist.size() - 1] == '/')
   {
      if (Dist != "/")
	 Info += Dist;
   }
   else
      Info += Dist + '/' + Section;   
   Info += " ";
   Info += Type;
   return Info;
}
									/*}}}*/
// SourcesIndex::Index* - Return the URI to the index files		/*{{{*/
// ---------------------------------------------------------------------
/* */
inline string debSourcesIndex::IndexFile(const char *Type) const
{
   string s = URItoFileName(IndexURI(Type));
   string sgzip = s + ".gz";
   if (!FileExists(s) && FileExists(sgzip))
       return sgzip;
   else
       return s;
}

string debSourcesIndex::IndexURI(const char *Type) const
{
   string Res;
   if (Dist[Dist.size() - 1] == '/')
   {
      if (Dist != "/")
	 Res = URI + Dist;
      else 
	 Res = URI;
   }
   else
      Res = URI + "dists/" + Dist + '/' + Section +
      "/source/";
   
   Res += Type;
   return Res;
}
									/*}}}*/
// SourcesIndex::Exists - Check if the index is available		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool debSourcesIndex::Exists() const
{
   return FileExists(IndexFile("Sources"));
}
									/*}}}*/
// SourcesIndex::Size - Return the size of the index			/*{{{*/
// ---------------------------------------------------------------------
/* */
unsigned long debSourcesIndex::Size() const
{
   unsigned long size = 0;

   /* we need to ignore errors here; if the lists are absent, just return 0 */
   _error->PushToStack();

   FileFd f = FileFd (IndexFile("Sources"), FileFd::ReadOnly, FileFd::Extension);
   if (!f.Failed())
      size = f.Size();

   if (_error->PendingError() == true)
       size = 0;
   _error->RevertToStack();

   return size;
}
									/*}}}*/

// PackagesIndex::debPackagesIndex - Contructor				/*{{{*/
// ---------------------------------------------------------------------
/* */
debPackagesIndex::debPackagesIndex(string const &URI, string const &Dist, string const &Section,
					bool const &Trusted, string const &Arch) :
                  pkgIndexFile(Trusted), URI(URI), Dist(Dist), Section(Section), Architecture(Arch)
{
	if (Architecture == "native")
		Architecture = _config->Find("APT::Architecture");
}
									/*}}}*/
// PackagesIndex::ArchiveInfo - Short version of the archive url	/*{{{*/
// ---------------------------------------------------------------------
/* This is a shorter version that is designed to be < 60 chars or so */
string debPackagesIndex::ArchiveInfo(pkgCache::VerIterator Ver) const
{
   string Res = ::URI::NoUserPassword(URI) + ' ';
   if (Dist[Dist.size() - 1] == '/')
   {
      if (Dist != "/")
	 Res += Dist;
   }
   else
      Res += Dist + '/' + Section;
   
   Res += " ";
   Res += Ver.ParentPkg().Name();
   Res += " ";
   if (Dist[Dist.size() - 1] != '/')
      Res.append(Ver.Arch()).append(" ");
   Res += Ver.VerStr();
   return Res;
}
									/*}}}*/
// PackagesIndex::Describe - Give a descriptive path to the index	/*{{{*/
// ---------------------------------------------------------------------
/* This should help the user find the index in the sources.list and
   in the filesystem for problem solving */
string debPackagesIndex::Describe(bool Short) const
{   
   char S[300];
   if (Short == true)
      snprintf(S,sizeof(S),"%s",Info("Packages").c_str());
   else
      snprintf(S,sizeof(S),"%s (%s)",Info("Packages").c_str(),
	       IndexFile("Packages").c_str());
   return S;
}
									/*}}}*/
// PackagesIndex::Info - One liner describing the index URI		/*{{{*/
// ---------------------------------------------------------------------
/* */
string debPackagesIndex::Info(const char *Type) const 
{
   string Info = ::URI::NoUserPassword(URI) + ' ';
   if (Dist[Dist.size() - 1] == '/')
   {
      if (Dist != "/")
	 Info += Dist;
   }
   else
      Info += Dist + '/' + Section;   
   Info += " ";
   if (Dist[Dist.size() - 1] != '/')
      Info += Architecture + " ";
   Info += Type;
   return Info;
}
									/*}}}*/
// PackagesIndex::Index* - Return the URI to the index files		/*{{{*/
// ---------------------------------------------------------------------
/* */
inline string debPackagesIndex::IndexFile(const char *Type) const
{
   string s =_config->FindDir("Dir::State::lists") + URItoFileName(IndexURI(Type));
   string sgzip = s + ".gz";
   if (!FileExists(s) && FileExists(sgzip))
       return sgzip;
   else
       return s;
}
string debPackagesIndex::IndexURI(const char *Type) const
{
   string Res;
   if (Dist[Dist.size() - 1] == '/')
   {
      if (Dist != "/")
	 Res = URI + Dist;
      else 
	 Res = URI;
   }
   else
      Res = URI + "dists/" + Dist + '/' + Section +
      "/binary-" + Architecture + '/';
   
   Res += Type;
   return Res;
}
									/*}}}*/
// PackagesIndex::Exists - Check if the index is available		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool debPackagesIndex::Exists() const
{
   return FileExists(IndexFile("Packages"));
}
									/*}}}*/
// PackagesIndex::Size - Return the size of the index			/*{{{*/
// ---------------------------------------------------------------------
/* This is really only used for progress reporting. */
unsigned long debPackagesIndex::Size() const
{
   unsigned long size = 0;

   /* we need to ignore errors here; if the lists are absent, just return 0 */
   _error->PushToStack();

   FileFd f = FileFd (IndexFile("Packages"), FileFd::ReadOnly, FileFd::Extension);
   if (!f.Failed())
      size = f.Size();

   if (_error->PendingError() == true)
       size = 0;
   _error->RevertToStack();

   return size;
}
									/*}}}*/
// PackagesIndex::Merge - Load the index file into a cache		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool debPackagesIndex::Merge(pkgCacheGenerator &Gen,OpProgress *Prog) const
{
   string PackageFile = IndexFile("Packages");
   FileFd Pkg(PackageFile,FileFd::ReadOnly, FileFd::Extension);
   debListParser Parser(&Pkg, Architecture);

   if (_error->PendingError() == true)
      return _error->Error("Problem opening %s",PackageFile.c_str());
   if (Prog != NULL)
      Prog->SubProgress(0,Info("Packages"));
   ::URI Tmp(URI);
   if (Gen.SelectFile(PackageFile,Tmp.Host,*this) == false)
      return _error->Error("Problem with SelectFile %s",PackageFile.c_str());

   // Store the IMS information
   pkgCache::PkgFileIterator File = Gen.GetCurFile();
   pkgCacheGenerator::Dynamic<pkgCache::PkgFileIterator> DynFile(File);
   File->Size = Pkg.FileSize();
   File->mtime = Pkg.ModificationTime();
   
   if (Gen.MergeList(Parser) == false)
      return _error->Error("Problem with MergeList %s",PackageFile.c_str());

   // Check the release file
   string ReleaseFile = debReleaseIndex(URI,Dist).MetaIndexFile("InRelease");
   bool releaseExists = false;
   if (FileExists(ReleaseFile) == true)
      releaseExists = true;
   else
      ReleaseFile = debReleaseIndex(URI,Dist).MetaIndexFile("Release");

   if (releaseExists == true || FileExists(ReleaseFile) == true)
   {
      FileFd Rel(ReleaseFile,FileFd::ReadOnly);
      if (_error->PendingError() == true)
	 return false;
      Parser.LoadReleaseInfo(File,Rel,Section);
   }
   
   return true;
}
									/*}}}*/
// PackagesIndex::FindInCache - Find this index				/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgCache::PkgFileIterator debPackagesIndex::FindInCache(pkgCache &Cache) const
{
   string FileName = IndexFile("Packages");
   pkgCache::PkgFileIterator File = Cache.FileBegin();
   for (; File.end() == false; ++File)
   {
       if (File.FileName() == NULL || FileName != File.FileName())
	 continue;
      
      struct stat St;
      if (stat(File.FileName(),&St) != 0)
      {
         if (_config->FindB("Debug::pkgCacheGen", false))
	    std::clog << "PackagesIndex::FindInCache - stat failed on " << File.FileName() << std::endl;
	 return pkgCache::PkgFileIterator(Cache);
      }
      if ((unsigned)St.st_size != File->Size || St.st_mtime != File->mtime)
      {
         if (_config->FindB("Debug::pkgCacheGen", false))
	    std::clog << "PackagesIndex::FindInCache - size (" << St.st_size << " <> " << File->Size
			<< ") or mtime (" << St.st_mtime << " <> " << File->mtime
			<< ") doesn't match for " << File.FileName() << std::endl;
	 return pkgCache::PkgFileIterator(Cache);
      }
      return File;
   }
   
   return File;
}
									/*}}}*/

// TranslationsIndex::debTranslationsIndex - Contructor			/*{{{*/
// ---------------------------------------------------------------------
/* */
debTranslationsIndex::debTranslationsIndex(string URI,string Dist,string Section,
						char const * const Translation) :
			pkgIndexFile(true), URI(URI), Dist(Dist), Section(Section),
				Language(Translation)
{}
									/*}}}*/
// TranslationIndex::Trans* - Return the URI to the translation files	/*{{{*/
// ---------------------------------------------------------------------
/* */
inline string debTranslationsIndex::IndexFile(const char *Type) const
{
   string s =_config->FindDir("Dir::State::lists") + URItoFileName(IndexURI(Type));
   string sgzip = s + ".gz";
   if (!FileExists(s) && FileExists(sgzip))
       return sgzip;
   else
       return s;
}
string debTranslationsIndex::IndexURI(const char *Type) const
{
   string Res;
   if (Dist[Dist.size() - 1] == '/')
   {
      if (Dist != "/")
	 Res = URI + Dist;
      else 
	 Res = URI;
   }
   else
      Res = URI + "dists/" + Dist + '/' + Section +
      "/i18n/Translation-";
   
   Res += Type;
   return Res;
}
									/*}}}*/
// TranslationsIndex::GetIndexes - Fetch the index files		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool debTranslationsIndex::GetIndexes(pkgAcquire *Owner) const
{
   string const TranslationFile = string("Translation-").append(Language);
   new pkgAcqIndexTrans(Owner, IndexURI(Language),
                        Info(TranslationFile.c_str()),
                        TranslationFile);

   return true;
}
									/*}}}*/
// TranslationsIndex::Describe - Give a descriptive path to the index	/*{{{*/
// ---------------------------------------------------------------------
/* This should help the user find the index in the sources.list and
   in the filesystem for problem solving */
string debTranslationsIndex::Describe(bool Short) const
{   
   char S[300];
   if (Short == true)
      snprintf(S,sizeof(S),"%s",Info(TranslationFile().c_str()).c_str());
   else
      snprintf(S,sizeof(S),"%s (%s)",Info(TranslationFile().c_str()).c_str(),
	       IndexFile(Language).c_str());
   return S;
}
									/*}}}*/
// TranslationsIndex::Info - One liner describing the index URI		/*{{{*/
// ---------------------------------------------------------------------
/* */
string debTranslationsIndex::Info(const char *Type) const 
{
   string Info = ::URI::NoUserPassword(URI) + ' ';
   if (Dist[Dist.size() - 1] == '/')
   {
      if (Dist != "/")
	 Info += Dist;
   }
   else
      Info += Dist + '/' + Section;   
   Info += " ";
   Info += Type;
   return Info;
}
									/*}}}*/
bool debTranslationsIndex::HasPackages() const				/*{{{*/
{
   return FileExists(IndexFile(Language));
}
									/*}}}*/
// TranslationsIndex::Exists - Check if the index is available		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool debTranslationsIndex::Exists() const
{
   return FileExists(IndexFile(Language));
}
									/*}}}*/
// TranslationsIndex::Size - Return the size of the index		/*{{{*/
// ---------------------------------------------------------------------
/* This is really only used for progress reporting. */
unsigned long debTranslationsIndex::Size() const
{
   unsigned long size = 0;

   /* we need to ignore errors here; if the lists are absent, just return 0 */
   _error->PushToStack();

   FileFd f = FileFd (IndexFile(Language), FileFd::ReadOnly, FileFd::Extension);
   if (!f.Failed())
      size = f.Size();

   if (_error->PendingError() == true)
       size = 0;
   _error->RevertToStack();

   return size;
}
									/*}}}*/
// TranslationsIndex::Merge - Load the index file into a cache		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool debTranslationsIndex::Merge(pkgCacheGenerator &Gen,OpProgress *Prog) const
{
   // Check the translation file, if in use
   string TranslationFile = IndexFile(Language);
   if (FileExists(TranslationFile))
   {
     FileFd Trans(TranslationFile,FileFd::ReadOnly, FileFd::Extension);
     debListParser TransParser(&Trans);
     if (_error->PendingError() == true)
       return false;
     
     if (Prog != NULL)
	Prog->SubProgress(0, Info(TranslationFile.c_str()));
     if (Gen.SelectFile(TranslationFile,string(),*this) == false)
       return _error->Error("Problem with SelectFile %s",TranslationFile.c_str());

     // Store the IMS information
     pkgCache::PkgFileIterator TransFile = Gen.GetCurFile();
     TransFile->Size = Trans.FileSize();
     TransFile->mtime = Trans.ModificationTime();
   
     if (Gen.MergeList(TransParser) == false)
       return _error->Error("Problem with MergeList %s",TranslationFile.c_str());
   }

   return true;
}
									/*}}}*/
// TranslationsIndex::FindInCache - Find this index				/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgCache::PkgFileIterator debTranslationsIndex::FindInCache(pkgCache &Cache) const
{
   string FileName = IndexFile(Language);
   
   pkgCache::PkgFileIterator File = Cache.FileBegin();
   for (; File.end() == false; ++File)
   {
      if (FileName != File.FileName())
	 continue;

      struct stat St;
      if (stat(File.FileName(),&St) != 0)
      {
         if (_config->FindB("Debug::pkgCacheGen", false))
	    std::clog << "TranslationIndex::FindInCache - stat failed on " << File.FileName() << std::endl;
	 return pkgCache::PkgFileIterator(Cache);
      }
      if ((unsigned)St.st_size != File->Size || St.st_mtime != File->mtime)
      {
         if (_config->FindB("Debug::pkgCacheGen", false))
	    std::clog << "TranslationIndex::FindInCache - size (" << St.st_size << " <> " << File->Size
			<< ") or mtime (" << St.st_mtime << " <> " << File->mtime
			<< ") doesn't match for " << File.FileName() << std::endl;
	 return pkgCache::PkgFileIterator(Cache);
      }
      return File;
   }   
   return File;
}
									/*}}}*/
// StatusIndex::debStatusIndex - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* */
debStatusIndex::debStatusIndex(string File) : pkgIndexFile(true), File(File)
{
}
									/*}}}*/
// StatusIndex::Size - Return the size of the index			/*{{{*/
// ---------------------------------------------------------------------
/* */
unsigned long debStatusIndex::Size() const
{
   struct stat S;
   if (stat(File.c_str(),&S) != 0)
      return 0;
   return S.st_size;
}
									/*}}}*/
// StatusIndex::Merge - Load the index file into a cache		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool debStatusIndex::Merge(pkgCacheGenerator &Gen,OpProgress *Prog) const
{
   FileFd Pkg(File,FileFd::ReadOnly, FileFd::Extension);
   if (_error->PendingError() == true)
      return false;
   debListParser Parser(&Pkg);
   if (_error->PendingError() == true)
      return false;

   if (Prog != NULL)
      Prog->SubProgress(0,File);
   if (Gen.SelectFile(File,string(),*this,pkgCache::Flag::NotSource) == false)
      return _error->Error("Problem with SelectFile %s",File.c_str());

   // Store the IMS information
   pkgCache::PkgFileIterator CFile = Gen.GetCurFile();
   CFile->Size = Pkg.FileSize();
   CFile->mtime = Pkg.ModificationTime();
   CFile->Archive = Gen.WriteUniqString("now");
   
   if (Gen.MergeList(Parser) == false)
      return _error->Error("Problem with MergeList %s",File.c_str());   
   return true;
}
									/*}}}*/
// StatusIndex::FindInCache - Find this index				/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgCache::PkgFileIterator debStatusIndex::FindInCache(pkgCache &Cache) const
{
   pkgCache::PkgFileIterator File = Cache.FileBegin();
   for (; File.end() == false; ++File)
   {
      if (this->File != File.FileName())
	 continue;
      
      struct stat St;
      if (stat(File.FileName(),&St) != 0)
      {
         if (_config->FindB("Debug::pkgCacheGen", false))
	    std::clog << "StatusIndex::FindInCache - stat failed on " << File.FileName() << std::endl;
	 return pkgCache::PkgFileIterator(Cache);
      }
      if ((unsigned)St.st_size != File->Size || St.st_mtime != File->mtime)
      {
         if (_config->FindB("Debug::pkgCacheGen", false))
	    std::clog << "StatusIndex::FindInCache - size (" << St.st_size << " <> " << File->Size
			<< ") or mtime (" << St.st_mtime << " <> " << File->mtime
			<< ") doesn't match for " << File.FileName() << std::endl;
	 return pkgCache::PkgFileIterator(Cache);
      }
      return File;
   }   
   return File;
}
									/*}}}*/
// StatusIndex::Exists - Check if the index is available		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool debStatusIndex::Exists() const
{
   // Abort if the file does not exist.
   return true;
}
									/*}}}*/

// Index File types for Debian						/*{{{*/
class debIFTypeSrc : public pkgIndexFile::Type
{
   public:
   
   debIFTypeSrc() {Label = "Debian Source Index";};
};
class debIFTypePkg : public pkgIndexFile::Type
{
   public:
   
   virtual pkgRecords::Parser *CreatePkgParser(pkgCache::PkgFileIterator File) const 
   {
      return new debRecordParser(File.FileName(),*File.Cache());
   };
   debIFTypePkg() {Label = "Debian Package Index";};
};
class debIFTypeTrans : public debIFTypePkg
{
   public:
   debIFTypeTrans() {Label = "Debian Translation Index";};
};
class debIFTypeStatus : public pkgIndexFile::Type
{
   public:
   
   virtual pkgRecords::Parser *CreatePkgParser(pkgCache::PkgFileIterator File) const 
   {
      return new debRecordParser(File.FileName(),*File.Cache());
   };
   debIFTypeStatus() {Label = "Debian dpkg status file";};
};
static debIFTypeSrc _apt_Src;
static debIFTypePkg _apt_Pkg;
static debIFTypeTrans _apt_Trans;
static debIFTypeStatus _apt_Status;

const pkgIndexFile::Type *debSourcesIndex::GetType() const
{
   return &_apt_Src;
}
const pkgIndexFile::Type *debPackagesIndex::GetType() const
{
   return &_apt_Pkg;
}
const pkgIndexFile::Type *debTranslationsIndex::GetType() const
{
   return &_apt_Trans;
}
const pkgIndexFile::Type *debStatusIndex::GetType() const
{
   return &_apt_Status;
}

									/*}}}*/
