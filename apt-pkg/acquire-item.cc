// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: acquire-item.cc,v 1.46.2.9 2004/01/16 18:51:11 mdz Exp $
/* ######################################################################

   Acquire Item - Item to acquire

   Each item can download to exactly one file at a time. This means you
   cannot create an item that fetches two uri's to two files at the same 
   time. The pkgAcqIndex class creates a second class upon instantiation
   to fetch the other index files because of this.

   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include <config.h>

#include <apt-pkg/acquire-item.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/aptconfiguration.h>
#include <apt-pkg/sourcelist.h>
#include <apt-pkg/error.h>
#include <apt-pkg/strutl.h>
#include <apt-pkg/fileutl.h>
#include <apt-pkg/md5.h>
#include <apt-pkg/sha1.h>
#include <apt-pkg/tagfile.h>
#include <apt-pkg/indexrecords.h>
#include <apt-pkg/metaindex.h>

#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string>
#include <sstream>
#include <stdio.h>
#include <ctime>

#include <apti18n.h>
									/*}}}*/

using namespace std;

// Acquire::Item::Item - Constructor					/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgAcquire::Item::Item(pkgAcquire *Owner) : Owner(Owner), FileSize(0),
                       PartialSize(0), Mode(0), ID(0), Complete(false), 
                       Local(false), QueueCounter(0)
{
   Owner->Add(this);
   Status = StatIdle;
}
									/*}}}*/
// Acquire::Item::~Item - Destructor					/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgAcquire::Item::~Item()
{
   Owner->Remove(this);
}
									/*}}}*/
// Acquire::Item::Failed - Item failed to download			/*{{{*/
// ---------------------------------------------------------------------
/* We return to an idle state if there are still other queues that could
   fetch this object */
void pkgAcquire::Item::Failed(string Message,pkgAcquire::MethodConfig *Cnf)
{
   Status = StatIdle;
   ErrorText = LookupTag(Message,"Message");
   UsedMirror =  LookupTag(Message,"UsedMirror");
   if (QueueCounter <= 1)
   {
      /* This indicates that the file is not available right now but might
         be sometime later. If we do a retry cycle then this should be
	 retried [CDROMs] */
      if (Cnf->LocalOnly == true &&
	  StringToBool(LookupTag(Message,"Transient-Failure"),false) == true)
      {
	 Status = StatIdle;
	 Dequeue();
	 return;
      }

      Status = StatError;
      Dequeue();
   }   

   // report mirror failure back to LP if we actually use a mirror
   string FailReason = LookupTag(Message, "FailReason");
   if(FailReason.size() != 0)
      ReportMirrorFailure(FailReason);
   else
      ReportMirrorFailure(ErrorText);
}
									/*}}}*/
// Acquire::Item::Start - Item has begun to download			/*{{{*/
// ---------------------------------------------------------------------
/* Stash status and the file size. Note that setting Complete means 
   sub-phases of the acquire process such as decompresion are operating */
void pkgAcquire::Item::Start(string /*Message*/,unsigned long long Size)
{
   Status = StatFetching;
   if (FileSize == 0 && Complete == false)
      FileSize = Size;
}
									/*}}}*/
// Acquire::Item::Done - Item downloaded OK				/*{{{*/
// ---------------------------------------------------------------------
/* */
void pkgAcquire::Item::Done(string Message,unsigned long long Size,string Hash,
			    pkgAcquire::MethodConfig *Cnf)
{
   // We just downloaded something..
   string FileName = LookupTag(Message,"Filename");
   UsedMirror =  LookupTag(Message,"UsedMirror");
   if (Complete == false && !Local && FileName == DestFile)
   {
      if (Owner->Log != 0)
	 Owner->Log->Fetched(Size,atoi(LookupTag(Message,"Resume-Point","0").c_str()));
   }

   if (FileSize == 0)
      FileSize= Size;
   Status = StatDone;
   ErrorText = string();
   Owner->Dequeue(this);
}
									/*}}}*/
// Acquire::Item::Rename - Rename a file				/*{{{*/
// ---------------------------------------------------------------------
/* This helper function is used by alot of item methods as thier final
   step */
void pkgAcquire::Item::Rename(string From,string To)
{
   if (rename(From.c_str(),To.c_str()) != 0)
   {
      char S[300];
      snprintf(S,sizeof(S),_("rename failed, %s (%s -> %s)."),strerror(errno),
	      From.c_str(),To.c_str());
      Status = StatError;
      ErrorText = S;
   }   
}
									/*}}}*/
// Acquire::Item::ReportMirrorFailure					/*{{{*/
// ---------------------------------------------------------------------
void pkgAcquire::Item::ReportMirrorFailure(string FailCode)
{
   // we only act if a mirror was used at all
   if(UsedMirror.empty())
      return;
#if 0
   std::cerr << "\nReportMirrorFailure: " 
	     << UsedMirror
	     << " Uri: " << DescURI()
	     << " FailCode: " 
	     << FailCode << std::endl;
#endif
   const char *Args[40];
   unsigned int i = 0;
   string report = _config->Find("Methods::Mirror::ProblemReporting", 
				 "/usr/lib/apt/apt-report-mirror-failure");
   if(!FileExists(report))
      return;
   Args[i++] = report.c_str();
   Args[i++] = UsedMirror.c_str();
   Args[i++] = DescURI().c_str();
   Args[i++] = FailCode.c_str();
   Args[i++] = NULL;
   pid_t pid = ExecFork();
   if(pid < 0) 
   {
      _error->Error("ReportMirrorFailure Fork failed");
      return;
   }
   else if(pid == 0) 
   {
      execvp(Args[0], (char**)Args);
      std::cerr << "Could not exec " << Args[0] << std::endl;
      _exit(100);
   }
   if(!ExecWait(pid, "report-mirror-failure")) 
   {
      _error->Warning("Couldn't report problem to '%s'",
		      _config->Find("Methods::Mirror::ProblemReporting").c_str());
   }
}
									/*}}}*/
// AcqSubIndex::AcqSubIndex - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* Get the Index file first and see if there are languages available
 * If so, create a pkgAcqIndexTrans for the found language(s).
 */
pkgAcqSubIndex::pkgAcqSubIndex(pkgAcquire *Owner, string const &URI,
				 string const &URIDesc, string const &ShortDesc,
				 HashString const &ExpectedHash)
   : Item(Owner), ExpectedHash(ExpectedHash)
{
   Debug = _config->FindB("Debug::pkgAcquire::SubIndex",false);

   DestFile = _config->FindDir("Dir::State::lists") + "partial/";
   DestFile += URItoFileName(URI);

   Desc.URI = URI;
   Desc.Description = URIDesc;
   Desc.Owner = this;
   Desc.ShortDesc = ShortDesc;

   QueueURI(Desc);

   if(Debug)
      std::clog << "pkgAcqSubIndex: " << Desc.URI << std::endl;
}
									/*}}}*/
// AcqSubIndex::Custom600Headers - Insert custom request headers	/*{{{*/
// ---------------------------------------------------------------------
/* The only header we use is the last-modified header. */
string pkgAcqSubIndex::Custom600Headers()
{
   string Final = _config->FindDir("Dir::State::lists");
   Final += URItoFileName(Desc.URI);

   struct stat Buf;
   if (stat(Final.c_str(),&Buf) != 0)
      return "\nIndex-File: true\nFail-Ignore: true\n";
   return "\nIndex-File: true\nFail-Ignore: true\nLast-Modified: " + TimeRFC1123(Buf.st_mtime);
}
									/*}}}*/
void pkgAcqSubIndex::Failed(string Message,pkgAcquire::MethodConfig *Cnf)	/*{{{*/
{
   if(Debug)
      std::clog << "pkgAcqSubIndex failed: " << Desc.URI << std::endl;

   Complete = false;
   Status = StatDone;
   Dequeue();

   // No good Index is provided, so try guessing
   std::vector<std::string> langs = APT::Configuration::getLanguages(true);
   for (std::vector<std::string>::const_iterator l = langs.begin();
	l != langs.end(); ++l)
   {
      if (*l == "none") continue;
      string const file = "Translation-" + *l;
      new pkgAcqIndexTrans(Owner, Desc.URI.substr(0, Desc.URI.rfind('/')+1).append(file),
		Desc.Description.erase(Desc.Description.rfind(' ')+1).append(file),
		file);
   }
}
									/*}}}*/
void pkgAcqSubIndex::Done(string Message,unsigned long long Size,string Md5Hash,	/*{{{*/
			   pkgAcquire::MethodConfig *Cnf)
{
   if(Debug)
      std::clog << "pkgAcqSubIndex::Done(): " << Desc.URI << std::endl;

   string FileName = LookupTag(Message,"Filename");
   if (FileName.empty() == true)
   {
      Status = StatError;
      ErrorText = "Method gave a blank filename";
      return;
   }

   if (FileName != DestFile)
   {
      Local = true;
      Desc.URI = "copy:" + FileName;
      QueueURI(Desc);
      return;
   }

   Item::Done(Message,Size,Md5Hash,Cnf);

   string FinalFile = _config->FindDir("Dir::State::lists")+URItoFileName(Desc.URI);

   /* Downloaded invalid transindex => Error (LP: #346386) (Closes: #627642) */
   indexRecords SubIndexParser;
   if (FileExists(DestFile) == true && !SubIndexParser.Load(DestFile)) {
      Status = StatError;
      ErrorText = SubIndexParser.ErrorText;
      return;
   }

   // sucess in downloading the index
   // rename the index
   if(Debug)
      std::clog << "Renaming: " << DestFile << " -> " << FinalFile << std::endl;
   Rename(DestFile,FinalFile);
   chmod(FinalFile.c_str(),0644);
   DestFile = FinalFile;

   if(ParseIndex(DestFile) == false)
      return Failed("", NULL);

   Complete = true;
   Status = StatDone;
   Dequeue();
   return;
}
									/*}}}*/
bool pkgAcqSubIndex::ParseIndex(string const &IndexFile)		/*{{{*/
{
   indexRecords SubIndexParser;
   if (FileExists(IndexFile) == false || SubIndexParser.Load(IndexFile) == false)
      return false;

   std::vector<std::string> lang = APT::Configuration::getLanguages(true);
   for (std::vector<std::string>::const_iterator l = lang.begin();
	l != lang.end(); ++l)
   {
      if (*l == "none")
	 continue;

      string file = "Translation-" + *l;
      indexRecords::checkSum const *Record = SubIndexParser.Lookup(file);
      HashString expected;
      if (Record == NULL)
      {
	 // FIXME: the Index file provided by debian currently only includes bz2 records
	 Record = SubIndexParser.Lookup(file + ".bz2");
	 if (Record == NULL)
	    continue;
      }
      else
      {
	 expected = Record->Hash;
	 if (expected.empty() == true)
	    continue;
      }

      IndexTarget target;
      target.Description = Desc.Description.erase(Desc.Description.rfind(' ')+1).append(file);
      target.MetaKey = file;
      target.ShortDesc = file;
      target.URI = Desc.URI.substr(0, Desc.URI.rfind('/')+1).append(file);
      new pkgAcqIndexTrans(Owner, &target, expected, &SubIndexParser);
   }
   return true;
}
									/*}}}*/
// AcqDiffIndex::AcqDiffIndex - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* Get the DiffIndex file first and see if there are patches availabe 
 * If so, create a pkgAcqIndexDiffs fetcher that will get and apply the
 * patches. If anything goes wrong in that process, it will fall back to
 * the original packages file
 */
pkgAcqDiffIndex::pkgAcqDiffIndex(pkgAcquire *Owner,
				 string URI,string URIDesc,string ShortDesc,
				 HashString ExpectedHash)
   : Item(Owner), RealURI(URI), ExpectedHash(ExpectedHash),
     Description(URIDesc)
{
   
   Debug = _config->FindB("Debug::pkgAcquire::Diffs",false);

   Desc.Description = URIDesc + "/DiffIndex";
   Desc.Owner = this;
   Desc.ShortDesc = ShortDesc;
   Desc.URI = URI + ".diff/Index";

   DestFile = _config->FindDir("Dir::State::lists") + "partial/";
   DestFile += URItoFileName(URI) + string(".DiffIndex");

   if(Debug)
      std::clog << "pkgAcqDiffIndex: " << Desc.URI << std::endl;

   // look for the current package file
   CurrentPackagesFile = _config->FindDir("Dir::State::lists");
   CurrentPackagesFile += URItoFileName(RealURI);

   // FIXME: this file:/ check is a hack to prevent fetching
   //        from local sources. this is really silly, and
   //        should be fixed cleanly as soon as possible
   if(!FileExists(CurrentPackagesFile) || 
      Desc.URI.substr(0,strlen("file:/")) == "file:/")
   {
      // we don't have a pkg file or we don't want to queue
      if(Debug)
	 std::clog << "No index file, local or canceld by user" << std::endl;
      Failed("", NULL);
      return;
   }

   if(Debug) 
      std::clog << "pkgAcqIndexDiffs::pkgAcqIndexDiffs(): " 
		<< CurrentPackagesFile << std::endl;
   
   QueueURI(Desc);

}
									/*}}}*/
// AcqIndex::Custom600Headers - Insert custom request headers		/*{{{*/
// ---------------------------------------------------------------------
/* The only header we use is the last-modified header. */
string pkgAcqDiffIndex::Custom600Headers()
{
   string Final = _config->FindDir("Dir::State::lists");
   Final += URItoFileName(RealURI) + string(".IndexDiff");
   
   if(Debug)
      std::clog << "Custom600Header-IMS: " << Final << std::endl;

   struct stat Buf;
   if (stat(Final.c_str(),&Buf) != 0)
      return "\nIndex-File: true";
   
   return "\nIndex-File: true\nLast-Modified: " + TimeRFC1123(Buf.st_mtime);
}
									/*}}}*/
bool pkgAcqDiffIndex::ParseDiffIndex(string IndexDiffFile)		/*{{{*/
{
   if(Debug)
      std::clog << "pkgAcqIndexDiffs::ParseIndexDiff() " << IndexDiffFile 
		<< std::endl;

   pkgTagSection Tags;
   string ServerSha1;
   vector<DiffInfo> available_patches;
   
   FileFd Fd(IndexDiffFile,FileFd::ReadOnly);
   pkgTagFile TF(&Fd);
   if (_error->PendingError() == true)
      return false;

   if(TF.Step(Tags) == true)
   {
      bool found = false;
      DiffInfo d;
      string size;

      string const tmp = Tags.FindS("SHA1-Current");
      std::stringstream ss(tmp);
      ss >> ServerSha1 >> size;
      unsigned long const ServerSize = atol(size.c_str());

      FileFd fd(CurrentPackagesFile, FileFd::ReadOnly);
      SHA1Summation SHA1;
      SHA1.AddFD(fd);
      string const local_sha1 = SHA1.Result();

      if(local_sha1 == ServerSha1) 
      {
	 // we have the same sha1 as the server
	 if(Debug)
	    std::clog << "Package file is up-to-date" << std::endl;
	 // set found to true, this will queue a pkgAcqIndexDiffs with
	 // a empty availabe_patches
	 found = true;
      } 
      else 
      {
	 if(Debug)
	    std::clog << "SHA1-Current: " << ServerSha1 << " and we start at "<< fd.Name() << " " << fd.Size() << " " << local_sha1 << std::endl;

	 // check the historie and see what patches we need
	 string const history = Tags.FindS("SHA1-History");
	 std::stringstream hist(history);
	 while(hist >> d.sha1 >> size >> d.file)
	 {
	    // read until the first match is found
	    // from that point on, we probably need all diffs
	    if(d.sha1 == local_sha1) 
	       found=true;
	    else if (found == false)
	       continue;

	    if(Debug)
	       std::clog << "Need to get diff: " << d.file << std::endl;
	    available_patches.push_back(d);
	 }

	 if (available_patches.empty() == false)
	 {
	    // patching with too many files is rather slow compared to a fast download
	    unsigned long const fileLimit = _config->FindI("Acquire::PDiffs::FileLimit", 0);
	    if (fileLimit != 0 && fileLimit < available_patches.size())
	    {
	       if (Debug)
		  std::clog << "Need " << available_patches.size() << " diffs (Limit is " << fileLimit
			<< ") so fallback to complete download" << std::endl;
	       return false;
	    }

	    // see if the patches are too big
	    found = false; // it was true and it will be true again at the end
	    d = *available_patches.begin();
	    string const firstPatch = d.file;
	    unsigned long patchesSize = 0;
	    std::stringstream patches(Tags.FindS("SHA1-Patches"));
	    while(patches >> d.sha1 >> size >> d.file)
	    {
	       if (firstPatch == d.file)
		  found = true;
	       else if (found == false)
		  continue;

	       patchesSize += atol(size.c_str());
	    }
	    unsigned long const sizeLimit = ServerSize * _config->FindI("Acquire::PDiffs::SizeLimit", 100);
	    if (sizeLimit > 0 && (sizeLimit/100) < patchesSize)
	    {
	       if (Debug)
		  std::clog << "Need " << patchesSize << " bytes (Limit is " << sizeLimit/100
			<< ") so fallback to complete download" << std::endl;
	       return false;
	    }
	 }
      }

      // we have something, queue the next diff
      if(found) 
      {
	 // queue the diffs
	 string::size_type const last_space = Description.rfind(" ");
	 if(last_space != string::npos)
	    Description.erase(last_space, Description.size()-last_space);
	 new pkgAcqIndexDiffs(Owner, RealURI, Description, Desc.ShortDesc,
			      ExpectedHash, ServerSha1, available_patches);
	 Complete = false;
	 Status = StatDone;
	 Dequeue();
	 return true;
      }
   }
   
   // Nothing found, report and return false
   // Failing here is ok, if we return false later, the full
   // IndexFile is queued
   if(Debug)
      std::clog << "Can't find a patch in the index file" << std::endl;
   return false;
}
									/*}}}*/
void pkgAcqDiffIndex::Failed(string Message,pkgAcquire::MethodConfig *Cnf)	/*{{{*/
{
   if(Debug)
      std::clog << "pkgAcqDiffIndex failed: " << Desc.URI << std::endl
		<< "Falling back to normal index file aquire" << std::endl;

   new pkgAcqIndex(Owner, RealURI, Description, Desc.ShortDesc, 
		   ExpectedHash);

   Complete = false;
   Status = StatDone;
   Dequeue();
}
									/*}}}*/
void pkgAcqDiffIndex::Done(string Message,unsigned long long Size,string Md5Hash,	/*{{{*/
			   pkgAcquire::MethodConfig *Cnf)
{
   if(Debug)
      std::clog << "pkgAcqDiffIndex::Done(): " << Desc.URI << std::endl;

   Item::Done(Message,Size,Md5Hash,Cnf);

   string FinalFile;
   FinalFile = _config->FindDir("Dir::State::lists")+URItoFileName(RealURI);

   // sucess in downloading the index
   // rename the index
   FinalFile += string(".IndexDiff");
   if(Debug)
      std::clog << "Renaming: " << DestFile << " -> " << FinalFile 
		<< std::endl;
   Rename(DestFile,FinalFile);
   chmod(FinalFile.c_str(),0644);
   DestFile = FinalFile;

   if(!ParseDiffIndex(DestFile))
      return Failed("", NULL);

   Complete = true;
   Status = StatDone;
   Dequeue();
   return;
}
									/*}}}*/
// AcqIndexDiffs::AcqIndexDiffs - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* The package diff is added to the queue. one object is constructed
 * for each diff and the index
 */
pkgAcqIndexDiffs::pkgAcqIndexDiffs(pkgAcquire *Owner,
				   string URI,string URIDesc,string ShortDesc,
				   HashString ExpectedHash, 
				   string ServerSha1,
				   vector<DiffInfo> diffs)
   : Item(Owner), RealURI(URI), ExpectedHash(ExpectedHash), 
     available_patches(diffs), ServerSha1(ServerSha1)
{
   
   DestFile = _config->FindDir("Dir::State::lists") + "partial/";
   DestFile += URItoFileName(URI);

   Debug = _config->FindB("Debug::pkgAcquire::Diffs",false);

   Description = URIDesc;
   Desc.Owner = this;
   Desc.ShortDesc = ShortDesc;

   if(available_patches.size() == 0) 
   {
      // we are done (yeah!)
      Finish(true);
   }
   else
   {
      // get the next diff
      State = StateFetchDiff;
      QueueNextDiff();
   }
}
									/*}}}*/
void pkgAcqIndexDiffs::Failed(string Message,pkgAcquire::MethodConfig *Cnf)	/*{{{*/
{
   if(Debug)
      std::clog << "pkgAcqIndexDiffs failed: " << Desc.URI << std::endl
		<< "Falling back to normal index file aquire" << std::endl;
   new pkgAcqIndex(Owner, RealURI, Description,Desc.ShortDesc, 
		   ExpectedHash);
   Finish();
}
									/*}}}*/
// Finish - helper that cleans the item out of the fetcher queue	/*{{{*/
void pkgAcqIndexDiffs::Finish(bool allDone)
{
   // we restore the original name, this is required, otherwise
   // the file will be cleaned
   if(allDone) 
   {
      DestFile = _config->FindDir("Dir::State::lists");
      DestFile += URItoFileName(RealURI);

      if(!ExpectedHash.empty() && !ExpectedHash.VerifyFile(DestFile))
      {
	 Status = StatAuthError;
	 ErrorText = _("MD5Sum mismatch");
	 Rename(DestFile,DestFile + ".FAILED");
	 Dequeue();
	 return;
      }

      // this is for the "real" finish
      Complete = true;
      Status = StatDone;
      Dequeue();
      if(Debug)
	 std::clog << "\n\nallDone: " << DestFile << "\n" << std::endl;
      return;
   }

   if(Debug)
      std::clog << "Finishing: " << Desc.URI << std::endl;
   Complete = false;
   Status = StatDone;
   Dequeue();
   return;
}
									/*}}}*/
bool pkgAcqIndexDiffs::QueueNextDiff()					/*{{{*/
{

   // calc sha1 of the just patched file
   string FinalFile = _config->FindDir("Dir::State::lists");
   FinalFile += URItoFileName(RealURI);

   FileFd fd(FinalFile, FileFd::ReadOnly);
   SHA1Summation SHA1;
   SHA1.AddFD(fd);
   string local_sha1 = string(SHA1.Result());
   if(Debug)
      std::clog << "QueueNextDiff: " 
		<< FinalFile << " (" << local_sha1 << ")"<<std::endl;

   // final file reached before all patches are applied
   if(local_sha1 == ServerSha1)
   {
      Finish(true);
      return true;
   }

   // remove all patches until the next matching patch is found
   // this requires the Index file to be ordered
   for(vector<DiffInfo>::iterator I=available_patches.begin();
       available_patches.empty() == false &&
	  I != available_patches.end() &&
	  I->sha1 != local_sha1;
       ++I)
   {
      available_patches.erase(I);
   }

   // error checking and falling back if no patch was found
   if(available_patches.empty() == true)
   {
      Failed("", NULL);
      return false;
   }

   // queue the right diff
   Desc.URI = string(RealURI) + ".diff/" + available_patches[0].file + ".gz";
   Desc.Description = Description + " " + available_patches[0].file + string(".pdiff");
   DestFile = _config->FindDir("Dir::State::lists") + "partial/";
   DestFile += URItoFileName(RealURI + ".diff/" + available_patches[0].file);

   if(Debug)
      std::clog << "pkgAcqIndexDiffs::QueueNextDiff(): " << Desc.URI << std::endl;
   
   QueueURI(Desc);

   return true;
}
									/*}}}*/
void pkgAcqIndexDiffs::Done(string Message,unsigned long long Size,string Md5Hash,	/*{{{*/
			    pkgAcquire::MethodConfig *Cnf)
{
   if(Debug)
      std::clog << "pkgAcqIndexDiffs::Done(): " << Desc.URI << std::endl;

   Item::Done(Message,Size,Md5Hash,Cnf);

   string FinalFile;
   FinalFile = _config->FindDir("Dir::State::lists")+URItoFileName(RealURI);

   // sucess in downloading a diff, enter ApplyDiff state
   if(State == StateFetchDiff)
   {

      // rred excepts the patch as $FinalFile.ed
      Rename(DestFile,FinalFile+".ed");

      if(Debug)
	 std::clog << "Sending to rred method: " << FinalFile << std::endl;

      State = StateApplyDiff;
      Local = true;
      Desc.URI = "rred:" + FinalFile;
      QueueURI(Desc);
      Mode = "rred";
      return;
   } 


   // success in download/apply a diff, queue next (if needed)
   if(State == StateApplyDiff)
   {
      // remove the just applied patch
      available_patches.erase(available_patches.begin());

      // move into place
      if(Debug) 
      {
	 std::clog << "Moving patched file in place: " << std::endl
		   << DestFile << " -> " << FinalFile << std::endl;
      }
      Rename(DestFile,FinalFile);
      chmod(FinalFile.c_str(),0644);

      // see if there is more to download
      if(available_patches.empty() == false) {
	 new pkgAcqIndexDiffs(Owner, RealURI, Description, Desc.ShortDesc,
			      ExpectedHash, ServerSha1, available_patches);
	 return Finish();
      } else 
	 return Finish(true);
   }
}
									/*}}}*/
// AcqIndex::AcqIndex - Constructor					/*{{{*/
// ---------------------------------------------------------------------
/* The package file is added to the queue and a second class is 
   instantiated to fetch the revision file */   
pkgAcqIndex::pkgAcqIndex(pkgAcquire *Owner,
			 string URI,string URIDesc,string ShortDesc,
			 HashString ExpectedHash, string comprExt)
   : Item(Owner), RealURI(URI), ExpectedHash(ExpectedHash)
{
   if(comprExt.empty() == true)
   {
      // autoselect the compression method
      std::vector<std::string> types = APT::Configuration::getCompressionTypes();
      for (std::vector<std::string>::const_iterator t = types.begin(); t != types.end(); ++t)
	 comprExt.append(*t).append(" ");
      if (comprExt.empty() == false)
	 comprExt.erase(comprExt.end()-1);
   }
   CompressionExtension = comprExt;

   Init(URI, URIDesc, ShortDesc);
}
pkgAcqIndex::pkgAcqIndex(pkgAcquire *Owner, IndexTarget const *Target,
			 HashString const &ExpectedHash, indexRecords const *MetaIndexParser)
   : Item(Owner), RealURI(Target->URI), ExpectedHash(ExpectedHash)
{
   // autoselect the compression method
   std::vector<std::string> types = APT::Configuration::getCompressionTypes();
   CompressionExtension = "";
   if (ExpectedHash.empty() == false)
   {
      for (std::vector<std::string>::const_iterator t = types.begin(); t != types.end(); ++t)
	 if (*t == "uncompressed" || MetaIndexParser->Exists(string(Target->MetaKey).append(".").append(*t)) == true)
	    CompressionExtension.append(*t).append(" ");
   }
   else
   {
      for (std::vector<std::string>::const_iterator t = types.begin(); t != types.end(); ++t)
	 CompressionExtension.append(*t).append(" ");
   }
   if (CompressionExtension.empty() == false)
      CompressionExtension.erase(CompressionExtension.end()-1);

   // only verify non-optional targets, see acquire-item.h for a FIXME
   // to make this more flexible
   if (Target->IsOptional())
     Verify = false;
   else
     Verify = true;

   Init(Target->URI, Target->Description, Target->ShortDesc);
}
									/*}}}*/
// AcqIndex::Init - defered Constructor					/*{{{*/
void pkgAcqIndex::Init(string const &URI, string const &URIDesc, string const &ShortDesc) {
   Decompression = false;
   Erase = false;

   DestFile = _config->FindDir("Dir::State::lists") + "partial/";
   DestFile += URItoFileName(URI);

   std::string const comprExt = CompressionExtension.substr(0, CompressionExtension.find(' '));
   if (comprExt == "uncompressed")
      Desc.URI = URI;
   else
      Desc.URI = URI + '.' + comprExt;

   Desc.Description = URIDesc;
   Desc.Owner = this;
   Desc.ShortDesc = ShortDesc;

   QueueURI(Desc);
}
									/*}}}*/
// AcqIndex::Custom600Headers - Insert custom request headers		/*{{{*/
// ---------------------------------------------------------------------
/* The only header we use is the last-modified header. */
string pkgAcqIndex::Custom600Headers()
{
   string Final = _config->FindDir("Dir::State::lists");
   Final += URItoFileName(RealURI);
   if (_config->FindB("Acquire::GzipIndexes",false))
      Final += ".gz";
   
   string msg = "\nIndex-File: true";
   // FIXME: this really should use "IndexTarget::IsOptional()" but that
   //        seems to be difficult without breaking ABI
   if (ShortDesc().find("Translation") != 0)
      msg += "\nFail-Ignore: true";
   struct stat Buf;
   if (stat(Final.c_str(),&Buf) == 0)
      msg += "\nLast-Modified: " + TimeRFC1123(Buf.st_mtime);

   return msg;
}
									/*}}}*/
void pkgAcqIndex::Failed(string Message,pkgAcquire::MethodConfig *Cnf)	/*{{{*/
{
   size_t const nextExt = CompressionExtension.find(' ');
   if (nextExt != std::string::npos)
   {
      CompressionExtension = CompressionExtension.substr(nextExt+1);
      Init(RealURI, Desc.Description, Desc.ShortDesc);
      return;
   }

   // on decompression failure, remove bad versions in partial/
   if (Decompression && Erase) {
      string s = _config->FindDir("Dir::State::lists") + "partial/";
      s.append(URItoFileName(RealURI));
      unlink(s.c_str());
   }

   Item::Failed(Message,Cnf);
}
									/*}}}*/
// AcqIndex::Done - Finished a fetch					/*{{{*/
// ---------------------------------------------------------------------
/* This goes through a number of states.. On the initial fetch the
   method could possibly return an alternate filename which points
   to the uncompressed version of the file. If this is so the file
   is copied into the partial directory. In all other cases the file
   is decompressed with a gzip uri. */
void pkgAcqIndex::Done(string Message,unsigned long long Size,string Hash,
		       pkgAcquire::MethodConfig *Cfg)
{
   Item::Done(Message,Size,Hash,Cfg);

   if (Decompression == true)
   {
      if (_config->FindB("Debug::pkgAcquire::Auth", false))
      {
         std::cerr << std::endl << RealURI << ": Computed Hash: " << Hash;
         std::cerr << "  Expected Hash: " << ExpectedHash.toStr() << std::endl;
      }

      if (!ExpectedHash.empty() && ExpectedHash.toStr() != Hash)
      {
         Status = StatAuthError;
         ErrorText = _("Hash Sum mismatch");
         Rename(DestFile,DestFile + ".FAILED");
	 ReportMirrorFailure("HashChecksumFailure");
         return;
      }

      /* Verify the index file for correctness (all indexes must
       * have a Package field) (LP: #346386) (Closes: #627642) */
      if (Verify == true)
      {
	 FileFd fd(DestFile, FileFd::ReadOnly);
	 pkgTagSection sec;
	 pkgTagFile tag(&fd);

         // Only test for correctness if the file is not empty (empty is ok)
         if (fd.Size() > 0) {
            if (_error->PendingError() || !tag.Step(sec)) {
               Status = StatError;
               _error->DumpErrors();
               Rename(DestFile,DestFile + ".FAILED");
               return;
            } else if (!sec.Exists("Package")) {
               Status = StatError;
               ErrorText = ("Encountered a section with no Package: header");
               Rename(DestFile,DestFile + ".FAILED");
               return;
            }
         }
      }
       
      // Done, move it into position
      string FinalFile = _config->FindDir("Dir::State::lists");
      FinalFile += URItoFileName(RealURI);
      Rename(DestFile,FinalFile);
      chmod(FinalFile.c_str(),0644);
      
      /* We restore the original name to DestFile so that the clean operation
         will work OK */
      DestFile = _config->FindDir("Dir::State::lists") + "partial/";
      DestFile += URItoFileName(RealURI);
      
      // Remove the compressed version.
      if (Erase == true)
	 unlink(DestFile.c_str());
      return;
   }

   Erase = false;
   Complete = true;
   
   // Handle the unzipd case
   string FileName = LookupTag(Message,"Alt-Filename");
   if (FileName.empty() == false)
   {
      // The files timestamp matches
      if (StringToBool(LookupTag(Message,"Alt-IMS-Hit"),false) == true)
	 return;
      Decompression = true;
      Local = true;
      DestFile += ".decomp";
      Desc.URI = "copy:" + FileName;
      QueueURI(Desc);
      Mode = "copy";
      return;
   }

   FileName = LookupTag(Message,"Filename");
   if (FileName.empty() == true)
   {
      Status = StatError;
      ErrorText = "Method gave a blank filename";
   }

   std::string const compExt = CompressionExtension.substr(0, CompressionExtension.find(' '));

   // The files timestamp matches
   if (StringToBool(LookupTag(Message,"IMS-Hit"),false) == true) {
       if (_config->FindB("Acquire::GzipIndexes",false) && compExt == "gz")
	  // Update DestFile for .gz suffix so that the clean operation keeps it
	  DestFile += ".gz";
      return;
    }

   if (FileName == DestFile)
      Erase = true;
   else
      Local = true;
   
   string decompProg;

   // If we enable compressed indexes and already have gzip, keep it
   if (_config->FindB("Acquire::GzipIndexes",false) && compExt == "gz" && !Local) {
      string FinalFile = _config->FindDir("Dir::State::lists");
      FinalFile += URItoFileName(RealURI) + ".gz";
      Rename(DestFile,FinalFile);
      chmod(FinalFile.c_str(),0644);
      
      // Update DestFile for .gz suffix so that the clean operation keeps it
      DestFile = _config->FindDir("Dir::State::lists") + "partial/";
      DestFile += URItoFileName(RealURI) + ".gz";
      return;
    }

   // get the binary name for your used compression type
   decompProg = _config->Find(string("Acquire::CompressionTypes::").append(compExt),"");
   if(decompProg.empty() == false);
   else if(compExt == "uncompressed")
      decompProg = "copy";
   else {
      _error->Error("Unsupported extension: %s", compExt.c_str());
      return;
   }

   Decompression = true;
   DestFile += ".decomp";
   Desc.URI = decompProg + ":" + FileName;
   QueueURI(Desc);
   Mode = decompProg.c_str();
}
									/*}}}*/
// AcqIndexTrans::pkgAcqIndexTrans - Constructor			/*{{{*/
// ---------------------------------------------------------------------
/* The Translation file is added to the queue */
pkgAcqIndexTrans::pkgAcqIndexTrans(pkgAcquire *Owner,
			    string URI,string URIDesc,string ShortDesc) 
  : pkgAcqIndex(Owner, URI, URIDesc, ShortDesc, HashString(), "")
{
}
pkgAcqIndexTrans::pkgAcqIndexTrans(pkgAcquire *Owner, IndexTarget const *Target,
			 HashString const &ExpectedHash, indexRecords const *MetaIndexParser)
  : pkgAcqIndex(Owner, Target, ExpectedHash, MetaIndexParser)
{
}
									/*}}}*/
// AcqIndexTrans::Custom600Headers - Insert custom request headers	/*{{{*/
// ---------------------------------------------------------------------
string pkgAcqIndexTrans::Custom600Headers()
{
   string Final = _config->FindDir("Dir::State::lists");
   Final += URItoFileName(RealURI);

   struct stat Buf;
   if (stat(Final.c_str(),&Buf) != 0)
      return "\nFail-Ignore: true\nIndex-File: true";
   return "\nFail-Ignore: true\nIndex-File: true\nLast-Modified: " + TimeRFC1123(Buf.st_mtime);
}
									/*}}}*/
// AcqIndexTrans::Failed - Silence failure messages for missing files	/*{{{*/
// ---------------------------------------------------------------------
/* */
void pkgAcqIndexTrans::Failed(string Message,pkgAcquire::MethodConfig *Cnf)
{
   size_t const nextExt = CompressionExtension.find(' ');
   if (nextExt != std::string::npos)
   {
      CompressionExtension = CompressionExtension.substr(nextExt+1);
      Init(RealURI, Desc.Description, Desc.ShortDesc);
      Status = StatIdle;
      return;
   }

   if (Cnf->LocalOnly == true || 
       StringToBool(LookupTag(Message,"Transient-Failure"),false) == false)
   {      
      // Ignore this
      Status = StatDone;
      Complete = false;
      Dequeue();
      return;
   }

   Item::Failed(Message,Cnf);
}
									/*}}}*/
pkgAcqMetaSig::pkgAcqMetaSig(pkgAcquire *Owner,				/*{{{*/
			     string URI,string URIDesc,string ShortDesc,
			     string MetaIndexURI, string MetaIndexURIDesc,
			     string MetaIndexShortDesc,
			     const vector<IndexTarget*>* IndexTargets,
			     indexRecords* MetaIndexParser) :
   Item(Owner), RealURI(URI), MetaIndexURI(MetaIndexURI),
   MetaIndexURIDesc(MetaIndexURIDesc), MetaIndexShortDesc(MetaIndexShortDesc),
   MetaIndexParser(MetaIndexParser), IndexTargets(IndexTargets)
{
   DestFile = _config->FindDir("Dir::State::lists") + "partial/";
   DestFile += URItoFileName(URI);

   // remove any partial downloaded sig-file in partial/. 
   // it may confuse proxies and is too small to warrant a 
   // partial download anyway
   unlink(DestFile.c_str());

   // Create the item
   Desc.Description = URIDesc;
   Desc.Owner = this;
   Desc.ShortDesc = ShortDesc;
   Desc.URI = URI;
      
   string Final = _config->FindDir("Dir::State::lists");
   Final += URItoFileName(RealURI);
   struct stat Buf;
   if (stat(Final.c_str(),&Buf) == 0)
   {
      // File was already in place.  It needs to be re-downloaded/verified
      // because Release might have changed, we do give it a differnt
      // name than DestFile because otherwise the http method will
      // send If-Range requests and there are too many broken servers
      // out there that do not understand them
      LastGoodSig = DestFile+".reverify";
      Rename(Final,LastGoodSig);
   }

   QueueURI(Desc);
}
									/*}}}*/
// pkgAcqMetaSig::Custom600Headers - Insert custom request headers	/*{{{*/
// ---------------------------------------------------------------------
/* The only header we use is the last-modified header. */
string pkgAcqMetaSig::Custom600Headers()
{
   struct stat Buf;
   if (stat(LastGoodSig.c_str(),&Buf) != 0)
      return "\nIndex-File: true";

   return "\nIndex-File: true\nLast-Modified: " + TimeRFC1123(Buf.st_mtime);
}

void pkgAcqMetaSig::Done(string Message,unsigned long long Size,string MD5,
			 pkgAcquire::MethodConfig *Cfg)
{
   Item::Done(Message,Size,MD5,Cfg);

   string FileName = LookupTag(Message,"Filename");
   if (FileName.empty() == true)
   {
      Status = StatError;
      ErrorText = "Method gave a blank filename";
      return;
   }

   if (FileName != DestFile)
   {
      // We have to copy it into place
      Local = true;
      Desc.URI = "copy:" + FileName;
      QueueURI(Desc);
      return;
   }

   Complete = true;

   // put the last known good file back on i-m-s hit (it will
   // be re-verified again)
   // Else do nothing, we have the new file in DestFile then
   if(StringToBool(LookupTag(Message,"IMS-Hit"),false) == true)
      Rename(LastGoodSig, DestFile);

   // queue a pkgAcqMetaIndex to be verified against the sig we just retrieved
   new pkgAcqMetaIndex(Owner, MetaIndexURI, MetaIndexURIDesc, 
		       MetaIndexShortDesc,  DestFile, IndexTargets, 
		       MetaIndexParser);

}
									/*}}}*/
void pkgAcqMetaSig::Failed(string Message,pkgAcquire::MethodConfig *Cnf)/*{{{*/
{
   string Final = _config->FindDir("Dir::State::lists") + URItoFileName(RealURI);

   // if we get a network error we fail gracefully
   if(Status == StatTransientNetworkError)
   {
      Item::Failed(Message,Cnf);
      // move the sigfile back on transient network failures 
      if(FileExists(LastGoodSig))
 	 Rename(LastGoodSig,Final);

      // set the status back to , Item::Failed likes to reset it
      Status = pkgAcquire::Item::StatTransientNetworkError;
      return;
   }

   // Delete any existing sigfile when the acquire failed
   unlink(Final.c_str());

   // queue a pkgAcqMetaIndex with no sigfile
   new pkgAcqMetaIndex(Owner, MetaIndexURI, MetaIndexURIDesc, MetaIndexShortDesc,
		       "", IndexTargets, MetaIndexParser);

   if (Cnf->LocalOnly == true || 
       StringToBool(LookupTag(Message,"Transient-Failure"),false) == false)
   {      
      // Ignore this
      Status = StatDone;
      Complete = false;
      Dequeue();
      return;
   }
   
   Item::Failed(Message,Cnf);
}
									/*}}}*/
pkgAcqMetaIndex::pkgAcqMetaIndex(pkgAcquire *Owner,			/*{{{*/
				 string URI,string URIDesc,string ShortDesc,
				 string SigFile,
				 const vector<struct IndexTarget*>* IndexTargets,
				 indexRecords* MetaIndexParser) :
   Item(Owner), RealURI(URI), SigFile(SigFile), IndexTargets(IndexTargets),
   MetaIndexParser(MetaIndexParser), AuthPass(false), IMSHit(false)
{
   DestFile = _config->FindDir("Dir::State::lists") + "partial/";
   DestFile += URItoFileName(URI);

   // Create the item
   Desc.Description = URIDesc;
   Desc.Owner = this;
   Desc.ShortDesc = ShortDesc;
   Desc.URI = URI;

   QueueURI(Desc);
}
									/*}}}*/
// pkgAcqMetaIndex::Custom600Headers - Insert custom request headers	/*{{{*/
// ---------------------------------------------------------------------
/* The only header we use is the last-modified header. */
string pkgAcqMetaIndex::Custom600Headers()
{
   string Final = _config->FindDir("Dir::State::lists");
   Final += URItoFileName(RealURI);
   
   struct stat Buf;
   if (stat(Final.c_str(),&Buf) != 0)
      return "\nIndex-File: true";
   
   return "\nIndex-File: true\nLast-Modified: " + TimeRFC1123(Buf.st_mtime);
}
									/*}}}*/
void pkgAcqMetaIndex::Done(string Message,unsigned long long Size,string Hash,	/*{{{*/
			   pkgAcquire::MethodConfig *Cfg)
{
   Item::Done(Message,Size,Hash,Cfg);

   // MetaIndexes are done in two passes: one to download the
   // metaindex with an appropriate method, and a second to verify it
   // with the gpgv method

   if (AuthPass == true)
   {
      AuthDone(Message);

      // all cool, move Release file into place
      Complete = true;
   }
   else
   {
      RetrievalDone(Message);
      if (!Complete)
         // Still more retrieving to do
         return;

      if (SigFile == "")
      {
         // There was no signature file, so we are finished.  Download
         // the indexes and do only hashsum verification if possible
         MetaIndexParser->Load(DestFile);
         QueueIndexes(false);
      }
      else
      {
         // There was a signature file, so pass it to gpgv for
         // verification

         if (_config->FindB("Debug::pkgAcquire::Auth", false))
            std::cerr << "Metaindex acquired, queueing gpg verification ("
                      << SigFile << "," << DestFile << ")\n";
         AuthPass = true;
         Desc.URI = "gpgv:" + SigFile;
         QueueURI(Desc);
         Mode = "gpgv";
	 return;
      }
   }

   if (Complete == true)
   {
      string FinalFile = _config->FindDir("Dir::State::lists");
      FinalFile += URItoFileName(RealURI);
      if (SigFile == DestFile)
	 SigFile = FinalFile;
      Rename(DestFile,FinalFile);
      chmod(FinalFile.c_str(),0644);
      DestFile = FinalFile;
   }
}
									/*}}}*/
void pkgAcqMetaIndex::RetrievalDone(string Message)			/*{{{*/
{
   // We have just finished downloading a Release file (it is not
   // verified yet)

   string FileName = LookupTag(Message,"Filename");
   if (FileName.empty() == true)
   {
      Status = StatError;
      ErrorText = "Method gave a blank filename";
      return;
   }

   if (FileName != DestFile)
   {
      Local = true;
      Desc.URI = "copy:" + FileName;
      QueueURI(Desc);
      return;
   }

   // make sure to verify against the right file on I-M-S hit
   IMSHit = StringToBool(LookupTag(Message,"IMS-Hit"),false);
   if(IMSHit)
   {
      string FinalFile = _config->FindDir("Dir::State::lists");
      FinalFile += URItoFileName(RealURI);
      if (SigFile == DestFile)
	 SigFile = FinalFile;
      DestFile = FinalFile;
   }
   Complete = true;
}
									/*}}}*/
void pkgAcqMetaIndex::AuthDone(string Message)				/*{{{*/
{
   // At this point, the gpgv method has succeeded, so there is a
   // valid signature from a key in the trusted keyring.  We
   // perform additional verification of its contents, and use them
   // to verify the indexes we are about to download

   if (!MetaIndexParser->Load(DestFile))
   {
      Status = StatAuthError;
      ErrorText = MetaIndexParser->ErrorText;
      return;
   }

   if (!VerifyVendor(Message))
   {
      return;
   }

   if (_config->FindB("Debug::pkgAcquire::Auth", false))
      std::cerr << "Signature verification succeeded: "
                << DestFile << std::endl;

   // Download further indexes with verification
   QueueIndexes(true);

   // is it a clearsigned MetaIndex file?
   if (DestFile == SigFile)
      return;

   // Done, move signature file into position
   string VerifiedSigFile = _config->FindDir("Dir::State::lists") +
      URItoFileName(RealURI) + ".gpg";
   Rename(SigFile,VerifiedSigFile);
   chmod(VerifiedSigFile.c_str(),0644);
}
									/*}}}*/
void pkgAcqMetaIndex::QueueIndexes(bool verify)				/*{{{*/
{
#if 0
   /* Reject invalid, existing Release files (LP: #346386) (Closes: #627642)
    * FIXME: Disabled; it breaks unsigned repositories without hashes */
   if (!verify && FileExists(DestFile) && !MetaIndexParser->Load(DestFile))
   {
      Status = StatError;
      ErrorText = MetaIndexParser->ErrorText;
      return;
   }
#endif
   for (vector <struct IndexTarget*>::const_iterator Target = IndexTargets->begin();
        Target != IndexTargets->end();
        ++Target)
   {
      HashString ExpectedIndexHash;
      const indexRecords::checkSum *Record = MetaIndexParser->Lookup((*Target)->MetaKey);
      if (Record == NULL)
      {
	 if (verify == true && (*Target)->IsOptional() == false)
	 {
	    Status = StatAuthError;
	    strprintf(ErrorText, _("Unable to find expected entry '%s' in Release file (Wrong sources.list entry or malformed file)"), (*Target)->MetaKey.c_str());
	    return;
	 }
      }
      else
      {
	 ExpectedIndexHash = Record->Hash;
	 if (_config->FindB("Debug::pkgAcquire::Auth", false))
	 {
	    std::cerr << "Queueing: " << (*Target)->URI << std::endl;
	    std::cerr << "Expected Hash: " << ExpectedIndexHash.toStr() << std::endl;
	    std::cerr << "For: " << Record->MetaKeyFilename << std::endl;
	 }
	 if (verify == true && ExpectedIndexHash.empty() == true && (*Target)->IsOptional() == false)
	 {
	    Status = StatAuthError;
	    strprintf(ErrorText, _("Unable to find hash sum for '%s' in Release file"), (*Target)->MetaKey.c_str());
	    return;
	 }
      }

      if ((*Target)->IsOptional() == true)
      {
	 if ((*Target)->IsSubIndex() == true)
	    new pkgAcqSubIndex(Owner, (*Target)->URI, (*Target)->Description,
				(*Target)->ShortDesc, ExpectedIndexHash);
	 else
	    new pkgAcqIndexTrans(Owner, *Target, ExpectedIndexHash, MetaIndexParser);
	 continue;
      }

      /* Queue Packages file (either diff or full packages files, depending
         on the users option) - we also check if the PDiff Index file is listed
         in the Meta-Index file. Ideal would be if pkgAcqDiffIndex would test this
         instead, but passing the required info to it is to much hassle */
      if(_config->FindB("Acquire::PDiffs",true) == true && (verify == false ||
	  MetaIndexParser->Exists(string((*Target)->MetaKey).append(".diff/Index")) == true))
	 new pkgAcqDiffIndex(Owner, (*Target)->URI, (*Target)->Description,
			     (*Target)->ShortDesc, ExpectedIndexHash);
      else
	 new pkgAcqIndex(Owner, *Target, ExpectedIndexHash, MetaIndexParser);
   }
}
									/*}}}*/
bool pkgAcqMetaIndex::VerifyVendor(string Message)			/*{{{*/
{
   string::size_type pos;

   // check for missing sigs (that where not fatal because otherwise we had
   // bombed earlier)
   string missingkeys;
   string msg = _("There is no public key available for the "
		  "following key IDs:\n");
   pos = Message.find("NO_PUBKEY ");
   if (pos != std::string::npos)
   {
      string::size_type start = pos+strlen("NO_PUBKEY ");
      string Fingerprint = Message.substr(start, Message.find("\n")-start);
      missingkeys += (Fingerprint);
   }
   if(!missingkeys.empty())
      _error->Warning("%s", string(msg+missingkeys).c_str());

   string Transformed = MetaIndexParser->GetExpectedDist();

   if (Transformed == "../project/experimental")
   {
      Transformed = "experimental";
   }

   pos = Transformed.rfind('/');
   if (pos != string::npos)
   {
      Transformed = Transformed.substr(0, pos);
   }

   if (Transformed == ".")
   {
      Transformed = "";
   }

   if (_config->FindB("Acquire::Check-Valid-Until", true) == true &&
       MetaIndexParser->GetValidUntil() > 0) {
      time_t const invalid_since = time(NULL) - MetaIndexParser->GetValidUntil();
      if (invalid_since > 0)
	 // TRANSLATOR: The first %s is the URL of the bad Release file, the second is
	 // the time since then the file is invalid - formated in the same way as in
	 // the download progress display (e.g. 7d 3h 42min 1s)
	 return _error->Error(
            _("Release file for %s is expired (invalid since %s). "
              "Updates for this repository will not be applied."),
            RealURI.c_str(), TimeToStr(invalid_since).c_str());
   }

   if (_config->FindB("Debug::pkgAcquire::Auth", false)) 
   {
      std::cerr << "Got Codename: " << MetaIndexParser->GetDist() << std::endl;
      std::cerr << "Expecting Dist: " << MetaIndexParser->GetExpectedDist() << std::endl;
      std::cerr << "Transformed Dist: " << Transformed << std::endl;
   }

   if (MetaIndexParser->CheckDist(Transformed) == false)
   {
      // This might become fatal one day
//       Status = StatAuthError;
//       ErrorText = "Conflicting distribution; expected "
//          + MetaIndexParser->GetExpectedDist() + " but got "
//          + MetaIndexParser->GetDist();
//       return false;
      if (!Transformed.empty())
      {
         _error->Warning(_("Conflicting distribution: %s (expected %s but got %s)"),
                         Desc.Description.c_str(),
                         Transformed.c_str(),
                         MetaIndexParser->GetDist().c_str());
      }
   }

   return true;
}
									/*}}}*/
// pkgAcqMetaIndex::Failed - no Release file present or no signature file present	/*{{{*/
// ---------------------------------------------------------------------
/* */
void pkgAcqMetaIndex::Failed(string Message,pkgAcquire::MethodConfig *Cnf)
{
   if (AuthPass == true)
   {
      // gpgv method failed, if we have a good signature 
      string LastGoodSigFile = _config->FindDir("Dir::State::lists");
      if (DestFile == SigFile)
	 LastGoodSigFile.append(URItoFileName(RealURI));
      else
	 LastGoodSigFile.append("partial/").append(URItoFileName(RealURI)).append(".gpg.reverify");

      if(FileExists(LastGoodSigFile))
      {
	 if (DestFile != SigFile)
	 {
	    string VerifiedSigFile = _config->FindDir("Dir::State::lists") +
					URItoFileName(RealURI) + ".gpg";
	    Rename(LastGoodSigFile,VerifiedSigFile);
	 }
	 Status = StatTransientNetworkError;
	 _error->Warning(_("A error occurred during the signature "
			   "verification. The repository is not updated "
			   "and the previous index files will be used. "
			   "GPG error: %s: %s\n"),
			 Desc.Description.c_str(),
			 LookupTag(Message,"Message").c_str());
	 RunScripts("APT::Update::Auth-Failure");
	 return;
      } else if (LookupTag(Message,"Message").find("NODATA") != string::npos) {
	 /* Invalid signature file, reject (LP: #346386) (Closes: #627642) */
	 _error->Error(_("GPG error: %s: %s"),
			 Desc.Description.c_str(),
			 LookupTag(Message,"Message").c_str());
	 return;
      } else {
	 _error->Warning(_("GPG error: %s: %s"),
			 Desc.Description.c_str(),
			 LookupTag(Message,"Message").c_str());
      }
      // gpgv method failed 
      ReportMirrorFailure("GPGFailure");
   }

   /* Always move the meta index, even if gpgv failed. This ensures
    * that PackageFile objects are correctly filled in */
   if (FileExists(DestFile)) {
      string FinalFile = _config->FindDir("Dir::State::lists");
      FinalFile += URItoFileName(RealURI);
      /* InRelease files become Release files, otherwise
       * they would be considered as trusted later on */
      if (SigFile == DestFile) {
	 RealURI = RealURI.replace(RealURI.rfind("InRelease"), 9,
	                               "Release");
	 FinalFile = FinalFile.replace(FinalFile.rfind("InRelease"), 9,
	                               "Release");
	 SigFile = FinalFile;
      }
      Rename(DestFile,FinalFile);
      chmod(FinalFile.c_str(),0644);

      DestFile = FinalFile;
   }

   // No Release file was present, or verification failed, so fall
   // back to queueing Packages files without verification
   QueueIndexes(false);
}
									/*}}}*/
pkgAcqMetaClearSig::pkgAcqMetaClearSig(pkgAcquire *Owner,		/*{{{*/
		string const &URI, string const &URIDesc, string const &ShortDesc,
		string const &MetaIndexURI, string const &MetaIndexURIDesc, string const &MetaIndexShortDesc,
		string const &MetaSigURI, string const &MetaSigURIDesc, string const &MetaSigShortDesc,
		const vector<struct IndexTarget*>* IndexTargets,
		indexRecords* MetaIndexParser) :
	pkgAcqMetaIndex(Owner, URI, URIDesc, ShortDesc, "", IndexTargets, MetaIndexParser),
	MetaIndexURI(MetaIndexURI), MetaIndexURIDesc(MetaIndexURIDesc), MetaIndexShortDesc(MetaIndexShortDesc),
	MetaSigURI(MetaSigURI), MetaSigURIDesc(MetaSigURIDesc), MetaSigShortDesc(MetaSigShortDesc)
{
   SigFile = DestFile;
}
									/*}}}*/
// pkgAcqMetaClearSig::Custom600Headers - Insert custom request headers	/*{{{*/
// ---------------------------------------------------------------------
// FIXME: this can go away once the InRelease file is used widely
string pkgAcqMetaClearSig::Custom600Headers()
{
   string Final = _config->FindDir("Dir::State::lists");
   Final += URItoFileName(RealURI);

   struct stat Buf;
   if (stat(Final.c_str(),&Buf) != 0)
      return "\nIndex-File: true\nFail-Ignore: true\n";

   return "\nIndex-File: true\nFail-Ignore: true\nLast-Modified: " + TimeRFC1123(Buf.st_mtime);
}
									/*}}}*/
void pkgAcqMetaClearSig::Failed(string Message,pkgAcquire::MethodConfig *Cnf) /*{{{*/
{
   if (AuthPass == false)
   {
      // Remove the 'old' InRelease file if we try Release.gpg now as otherwise
      // the file will stay around and gives a false-auth impression (CVE-2012-0214)
      string FinalFile = _config->FindDir("Dir::State::lists");
      FinalFile.append(URItoFileName(RealURI));
      if (FileExists(FinalFile))
	 unlink(FinalFile.c_str());

      new pkgAcqMetaSig(Owner,
			MetaSigURI, MetaSigURIDesc, MetaSigShortDesc,
			MetaIndexURI, MetaIndexURIDesc, MetaIndexShortDesc,
			IndexTargets, MetaIndexParser);
      if (Cnf->LocalOnly == true ||
	  StringToBool(LookupTag(Message, "Transient-Failure"), false) == false)
	 Dequeue();
   }
   else
      pkgAcqMetaIndex::Failed(Message, Cnf);
}
									/*}}}*/
// AcqArchive::AcqArchive - Constructor					/*{{{*/
// ---------------------------------------------------------------------
/* This just sets up the initial fetch environment and queues the first
   possibilitiy */
pkgAcqArchive::pkgAcqArchive(pkgAcquire *Owner,pkgSourceList *Sources,
			     pkgRecords *Recs,pkgCache::VerIterator const &Version,
			     string &StoreFilename) :
               Item(Owner), Version(Version), Sources(Sources), Recs(Recs), 
               StoreFilename(StoreFilename), Vf(Version.FileList()), 
	       Trusted(false)
{
   Retries = _config->FindI("Acquire::Retries",0);

   if (Version.Arch() == 0)
   {
      _error->Error(_("I wasn't able to locate a file for the %s package. "
		      "This might mean you need to manually fix this package. "
		      "(due to missing arch)"),
		    Version.ParentPkg().Name());
      return;
   }
   
   /* We need to find a filename to determine the extension. We make the
      assumption here that all the available sources for this version share
      the same extension.. */
   // Skip not source sources, they do not have file fields.
   for (; Vf.end() == false; Vf++)
   {
      if ((Vf.File()->Flags & pkgCache::Flag::NotSource) != 0)
	 continue;
      break;
   }
   
   // Does not really matter here.. we are going to fail out below
   if (Vf.end() != true)
   {     
      // If this fails to get a file name we will bomb out below.
      pkgRecords::Parser &Parse = Recs->Lookup(Vf);
      if (_error->PendingError() == true)
	 return;
            
      // Generate the final file name as: package_version_arch.foo
      StoreFilename = QuoteString(Version.ParentPkg().Name(),"_:") + '_' +
	              QuoteString(Version.VerStr(),"_:") + '_' +
     	              QuoteString(Version.Arch(),"_:.") + 
	              "." + flExtension(Parse.FileName());
   }

   // check if we have one trusted source for the package. if so, switch
   // to "TrustedOnly" mode
   for (pkgCache::VerFileIterator i = Version.FileList(); i.end() == false; ++i)
   {
      pkgIndexFile *Index;
      if (Sources->FindIndex(i.File(),Index) == false)
         continue;
      if (_config->FindB("Debug::pkgAcquire::Auth", false))
      {
         std::cerr << "Checking index: " << Index->Describe()
                   << "(Trusted=" << Index->IsTrusted() << ")\n";
      }
      if (Index->IsTrusted()) {
         Trusted = true;
	 break;
      }
   }

   // "allow-unauthenticated" restores apts old fetching behaviour
   // that means that e.g. unauthenticated file:// uris are higher
   // priority than authenticated http:// uris
   if (_config->FindB("APT::Get::AllowUnauthenticated",false) == true)
      Trusted = false;

   // Select a source
   if (QueueNext() == false && _error->PendingError() == false)
      _error->Error(_("I wasn't able to locate a file for the %s package. "
		    "This might mean you need to manually fix this package."),
		    Version.ParentPkg().Name());
}
									/*}}}*/
// AcqArchive::QueueNext - Queue the next file source			/*{{{*/
// ---------------------------------------------------------------------
/* This queues the next available file version for download. It checks if
   the archive is already available in the cache and stashs the MD5 for
   checking later. */
bool pkgAcqArchive::QueueNext()
{
   string const ForceHash = _config->Find("Acquire::ForceHash");
   for (; Vf.end() == false; ++Vf)
   {
      // Ignore not source sources
      if ((Vf.File()->Flags & pkgCache::Flag::NotSource) != 0)
	 continue;

      // Try to cross match against the source list
      pkgIndexFile *Index;
      if (Sources->FindIndex(Vf.File(),Index) == false)
	    continue;
      
      // only try to get a trusted package from another source if that source
      // is also trusted
      if(Trusted && !Index->IsTrusted()) 
	 continue;

      // Grab the text package record
      pkgRecords::Parser &Parse = Recs->Lookup(Vf);
      if (_error->PendingError() == true)
	 return false;
      
      string PkgFile = Parse.FileName();
      if (ForceHash.empty() == false)
      {
	 if(stringcasecmp(ForceHash, "sha512") == 0)
	    ExpectedHash = HashString("SHA512", Parse.SHA512Hash());
	 if(stringcasecmp(ForceHash, "sha256") == 0)
	    ExpectedHash = HashString("SHA256", Parse.SHA256Hash());
	 else if (stringcasecmp(ForceHash, "sha1") == 0)
	    ExpectedHash = HashString("SHA1", Parse.SHA1Hash());
	 else
	    ExpectedHash = HashString("MD5Sum", Parse.MD5Hash());
      }
      else
      {
	 string Hash;
	 if ((Hash = Parse.SHA512Hash()).empty() == false)
	    ExpectedHash = HashString("SHA512", Hash);
	 else if ((Hash = Parse.SHA256Hash()).empty() == false)
	    ExpectedHash = HashString("SHA256", Hash);
	 else if ((Hash = Parse.SHA1Hash()).empty() == false)
	    ExpectedHash = HashString("SHA1", Hash);
	 else
	    ExpectedHash = HashString("MD5Sum", Parse.MD5Hash());
      }
      if (PkgFile.empty() == true)
	 return _error->Error(_("The package index files are corrupted. No Filename: "
			      "field for package %s."),
			      Version.ParentPkg().Name());

      Desc.URI = Index->ArchiveURI(PkgFile);
      Desc.Description = Index->ArchiveInfo(Version);
      Desc.Owner = this;
      Desc.ShortDesc = Version.ParentPkg().Name();

      // See if we already have the file. (Legacy filenames)
      FileSize = Version->Size;
      string FinalFile = _config->FindDir("Dir::Cache::Archives") + flNotDir(PkgFile);
      struct stat Buf;
      if (stat(FinalFile.c_str(),&Buf) == 0)
      {
	 // Make sure the size matches
	 if ((unsigned long long)Buf.st_size == Version->Size)
	 {
	    Complete = true;
	    Local = true;
	    Status = StatDone;
	    StoreFilename = DestFile = FinalFile;
	    return true;
	 }
	 
	 /* Hmm, we have a file and its size does not match, this means it is
	    an old style mismatched arch */
	 unlink(FinalFile.c_str());
      }

      // Check it again using the new style output filenames
      FinalFile = _config->FindDir("Dir::Cache::Archives") + flNotDir(StoreFilename);
      if (stat(FinalFile.c_str(),&Buf) == 0)
      {
	 // Make sure the size matches
	 if ((unsigned long long)Buf.st_size == Version->Size)
	 {
	    Complete = true;
	    Local = true;
	    Status = StatDone;
	    StoreFilename = DestFile = FinalFile;
	    return true;
	 }
	 
	 /* Hmm, we have a file and its size does not match, this shouldnt
	    happen.. */
	 unlink(FinalFile.c_str());
      }

      DestFile = _config->FindDir("Dir::Cache::Archives") + "partial/" + flNotDir(StoreFilename);
      
      // Check the destination file
      if (stat(DestFile.c_str(),&Buf) == 0)
      {
	 // Hmm, the partial file is too big, erase it
	 if ((unsigned long long)Buf.st_size > Version->Size)
	    unlink(DestFile.c_str());
	 else
	    PartialSize = Buf.st_size;
      }

      // Disables download of archives - useful if no real installation follows,
      // e.g. if we are just interested in proposed installation order
      if (_config->FindB("Debug::pkgAcqArchive::NoQueue", false) == true)
      {
	 Complete = true;
	 Local = true;
	 Status = StatDone;
	 StoreFilename = DestFile = FinalFile;
	 return true;
      }

      // Create the item
      Local = false;
      Desc.URI = Index->ArchiveURI(PkgFile);
      Desc.Description = Index->ArchiveInfo(Version);
      Desc.Owner = this;
      Desc.ShortDesc = Version.ParentPkg().Name();
      QueueURI(Desc);

      ++Vf;
      return true;
   }
   return false;
}   
									/*}}}*/
// AcqArchive::Done - Finished fetching					/*{{{*/
// ---------------------------------------------------------------------
/* */
void pkgAcqArchive::Done(string Message,unsigned long long Size,string CalcHash,
			 pkgAcquire::MethodConfig *Cfg)
{
   Item::Done(Message,Size,CalcHash,Cfg);
   
   // Check the size
   if (Size != Version->Size)
   {
      Status = StatError;
      ErrorText = _("Size mismatch");
      return;
   }
   
   // Check the hash
   if(ExpectedHash.toStr() != CalcHash)
   {
      Status = StatError;
      ErrorText = _("Hash Sum mismatch");
      if(FileExists(DestFile))
	 Rename(DestFile,DestFile + ".FAILED");
      return;
   }

   // Grab the output filename
   string FileName = LookupTag(Message,"Filename");
   if (FileName.empty() == true)
   {
      Status = StatError;
      ErrorText = "Method gave a blank filename";
      return;
   }

   Complete = true;

   // Reference filename
   if (FileName != DestFile)
   {
      StoreFilename = DestFile = FileName;
      Local = true;
      return;
   }
   
   // Done, move it into position
   string FinalFile = _config->FindDir("Dir::Cache::Archives");
   FinalFile += flNotDir(StoreFilename);
   Rename(DestFile,FinalFile);
   
   StoreFilename = DestFile = FinalFile;
   Complete = true;
}
									/*}}}*/
// AcqArchive::Failed - Failure handler					/*{{{*/
// ---------------------------------------------------------------------
/* Here we try other sources */
void pkgAcqArchive::Failed(string Message,pkgAcquire::MethodConfig *Cnf)
{
   ErrorText = LookupTag(Message,"Message");
   
   /* We don't really want to retry on failed media swaps, this prevents 
      that. An interesting observation is that permanent failures are not
      recorded. */
   if (Cnf->Removable == true && 
       StringToBool(LookupTag(Message,"Transient-Failure"),false) == true)
   {
      // Vf = Version.FileList();
      while (Vf.end() == false) ++Vf;
      StoreFilename = string();
      Item::Failed(Message,Cnf);
      return;
   }
   
   if (QueueNext() == false)
   {
      // This is the retry counter
      if (Retries != 0 &&
	  Cnf->LocalOnly == false &&
	  StringToBool(LookupTag(Message,"Transient-Failure"),false) == true)
      {
	 Retries--;
	 Vf = Version.FileList();
	 if (QueueNext() == true)
	    return;
      }
      
      StoreFilename = string();
      Item::Failed(Message,Cnf);
   }
}
									/*}}}*/
// AcqArchive::IsTrusted - Determine whether this archive comes from a trusted source /*{{{*/
// ---------------------------------------------------------------------
bool pkgAcqArchive::IsTrusted()
{
   return Trusted;
}
									/*}}}*/
// AcqArchive::Finished - Fetching has finished, tidy up		/*{{{*/
// ---------------------------------------------------------------------
/* */
void pkgAcqArchive::Finished()
{
   if (Status == pkgAcquire::Item::StatDone &&
       Complete == true)
      return;
   StoreFilename = string();
}
									/*}}}*/
// AcqFile::pkgAcqFile - Constructor					/*{{{*/
// ---------------------------------------------------------------------
/* The file is added to the queue */
pkgAcqFile::pkgAcqFile(pkgAcquire *Owner,string URI,string Hash,
		       unsigned long long Size,string Dsc,string ShortDesc,
		       const string &DestDir, const string &DestFilename,
                       bool IsIndexFile) :
                       Item(Owner), ExpectedHash(Hash), IsIndexFile(IsIndexFile)
{
   Retries = _config->FindI("Acquire::Retries",0);
   
   if(!DestFilename.empty())
      DestFile = DestFilename;
   else if(!DestDir.empty())
      DestFile = DestDir + "/" + flNotDir(URI);
   else
      DestFile = flNotDir(URI);

   // Create the item
   Desc.URI = URI;
   Desc.Description = Dsc;
   Desc.Owner = this;

   // Set the short description to the archive component
   Desc.ShortDesc = ShortDesc;
      
   // Get the transfer sizes
   FileSize = Size;
   struct stat Buf;
   if (stat(DestFile.c_str(),&Buf) == 0)
   {
      // Hmm, the partial file is too big, erase it
      if ((unsigned long long)Buf.st_size > Size)
	 unlink(DestFile.c_str());
      else
	 PartialSize = Buf.st_size;
   }

   QueueURI(Desc);
}
									/*}}}*/
// AcqFile::Done - Item downloaded OK					/*{{{*/
// ---------------------------------------------------------------------
/* */
void pkgAcqFile::Done(string Message,unsigned long long Size,string CalcHash,
		      pkgAcquire::MethodConfig *Cnf)
{
   Item::Done(Message,Size,CalcHash,Cnf);

   // Check the hash
   if(!ExpectedHash.empty() && ExpectedHash.toStr() != CalcHash)
   {
      Status = StatError;
      ErrorText = _("Hash Sum mismatch");
      Rename(DestFile,DestFile + ".FAILED");
      return;
   }
   
   string FileName = LookupTag(Message,"Filename");
   if (FileName.empty() == true)
   {
      Status = StatError;
      ErrorText = "Method gave a blank filename";
      return;
   }

   Complete = true;
   
   // The files timestamp matches
   if (StringToBool(LookupTag(Message,"IMS-Hit"),false) == true)
      return;
   
   // We have to copy it into place
   if (FileName != DestFile)
   {
      Local = true;
      if (_config->FindB("Acquire::Source-Symlinks",true) == false ||
	  Cnf->Removable == true)
      {
	 Desc.URI = "copy:" + FileName;
	 QueueURI(Desc);
	 return;
      }
      
      // Erase the file if it is a symlink so we can overwrite it
      struct stat St;
      if (lstat(DestFile.c_str(),&St) == 0)
      {
	 if (S_ISLNK(St.st_mode) != 0)
	    unlink(DestFile.c_str());
      }
      
      // Symlink the file
      if (symlink(FileName.c_str(),DestFile.c_str()) != 0)
      {
	 ErrorText = "Link to " + DestFile + " failure ";
	 Status = StatError;
	 Complete = false;
      }      
   }
}
									/*}}}*/
// AcqFile::Failed - Failure handler					/*{{{*/
// ---------------------------------------------------------------------
/* Here we try other sources */
void pkgAcqFile::Failed(string Message,pkgAcquire::MethodConfig *Cnf)
{
   ErrorText = LookupTag(Message,"Message");
   
   // This is the retry counter
   if (Retries != 0 &&
       Cnf->LocalOnly == false &&
       StringToBool(LookupTag(Message,"Transient-Failure"),false) == true)
   {
      Retries--;
      QueueURI(Desc);
      return;
   }
   
   Item::Failed(Message,Cnf);
}
									/*}}}*/
// AcqIndex::Custom600Headers - Insert custom request headers		/*{{{*/
// ---------------------------------------------------------------------
/* The only header we use is the last-modified header. */
string pkgAcqFile::Custom600Headers()
{
   if (IsIndexFile)
      return "\nIndex-File: true";
   return "";
}
									/*}}}*/
