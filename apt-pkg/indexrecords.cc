// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: indexrecords.cc,v 1.1.2.4 2003/12/30 02:11:43 mdz Exp $
									/*}}}*/
// Include Files							/*{{{*/
#include<config.h>

#include <apt-pkg/indexrecords.h>
#include <apt-pkg/tagfile.h>
#include <apt-pkg/error.h>
#include <apt-pkg/strutl.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/fileutl.h>
#include <apt-pkg/hashes.h>

#include <sys/stat.h>
#include <clocale>

#include <apti18n.h>
									/*}}}*/

using std::string;

string indexRecords::GetDist() const
{
   return this->Dist;
}

bool indexRecords::CheckDist(const string MaybeDist) const
{
   return (this->Dist == MaybeDist
	   || this->Suite == MaybeDist);
}

string indexRecords::GetExpectedDist() const
{
   return this->ExpectedDist;
}

time_t indexRecords::GetValidUntil() const
{
   return this->ValidUntil;
}

const indexRecords::checkSum *indexRecords::Lookup(const string MetaKey)
{
   return Entries[MetaKey];
}

bool indexRecords::Exists(string const &MetaKey) const
{
   return Entries.count(MetaKey) == 1;
}

bool indexRecords::Load(const string Filename)				/*{{{*/
{
   FileFd Fd(Filename, FileFd::ReadOnly);
   pkgTagFile TagFile(&Fd, Fd.Size() + 256); // XXX
   if (_error->PendingError() == true)
   {
      strprintf(ErrorText, _("Unable to parse Release file %s"),Filename.c_str());
      return false;
   }

   pkgTagSection Section;
   const char *Start, *End;
   // Skip over sections beginning with ----- as this is an idicator for clearsigns
   do {
      if (TagFile.Step(Section) == false)
      {
	 strprintf(ErrorText, _("No sections in Release file %s"), Filename.c_str());
	 return false;
      }

      Section.Get (Start, End, 0);
   } while (End - Start > 5 && strncmp(Start, "-----", 5) == 0);

   Suite = Section.FindS("Suite");
   Dist = Section.FindS("Codename");

   int i;
   for (i=0;HashString::SupportedHashes()[i] != NULL; i++)
   {
      if (!Section.Find(HashString::SupportedHashes()[i], Start, End))
	 continue;

      string Name;
      string Hash;
      unsigned long long Size;
      while (Start < End)
      {
	 if (!parseSumData(Start, End, Name, Hash, Size))
	    return false;
	 indexRecords::checkSum *Sum = new indexRecords::checkSum;
	 Sum->MetaKeyFilename = Name;
	 Sum->Hash = HashString(HashString::SupportedHashes()[i],Hash);
	 Sum->Size = Size;
	 Entries[Name] = Sum;
      }
      break;
   }

   if(HashString::SupportedHashes()[i] == NULL)
   {
      strprintf(ErrorText, _("No Hash entry in Release file %s"), Filename.c_str());
      return false;
   }

   string Label = Section.FindS("Label");
   string StrDate = Section.FindS("Date");
   string StrValidUntil = Section.FindS("Valid-Until");

   // if we have a Valid-Until header in the Release file, use it as default
   if (StrValidUntil.empty() == false)
   {
      if(RFC1123StrToTime(StrValidUntil.c_str(), ValidUntil) == false)
      {
	 strprintf(ErrorText, _("Invalid 'Valid-Until' entry in Release file %s"), Filename.c_str());
	 return false;
      }
   }
   // get the user settings for this archive and use what expires earlier
   int MaxAge = _config->FindI("Acquire::Max-ValidTime", 0);
   if (Label.empty() == false)
      MaxAge = _config->FindI(string("Acquire::Max-ValidTime::" + Label).c_str(), MaxAge);
   int MinAge = _config->FindI("Acquire::Min-ValidTime", 0);
   if (Label.empty() == false)
      MinAge = _config->FindI(string("Acquire::Min-ValidTime::" + Label).c_str(), MinAge);

   if(MaxAge == 0 &&
      (MinAge == 0 || ValidUntil == 0)) // No user settings, use the one from the Release file
      return true;

   time_t date;
   if (RFC1123StrToTime(StrDate.c_str(), date) == false)
   {
      strprintf(ErrorText, _("Invalid 'Date' entry in Release file %s"), Filename.c_str());
      return false;
   }

   if (MinAge != 0 && ValidUntil != 0) {
      time_t const min_date = date + MinAge;
      if (ValidUntil < min_date)
	 ValidUntil = min_date;
   }
   if (MaxAge != 0) {
      time_t const max_date = date + MaxAge;
      if (ValidUntil == 0 || ValidUntil > max_date)
	 ValidUntil = max_date;
   }

   return true;
}
									/*}}}*/
std::vector<string> indexRecords::MetaKeys()				/*{{{*/
{
   std::vector<std::string> keys;
   std::map<string,checkSum *>::iterator I = Entries.begin();
   while(I != Entries.end()) {
      keys.push_back((*I).first);
      ++I;
   }
   return keys;
}
									/*}}}*/
bool indexRecords::parseSumData(const char *&Start, const char *End,	/*{{{*/
				   string &Name, string &Hash, unsigned long long &Size)
{
   Name = "";
   Hash = "";
   Size = 0;
   /* Skip over the first blank */
   while ((*Start == '\t' || *Start == ' ' || *Start == '\n')
	  && Start < End)
      Start++;
   if (Start >= End)
      return false;

   /* Move EntryEnd to the end of the first entry (the hash) */
   const char *EntryEnd = Start;
   while ((*EntryEnd != '\t' && *EntryEnd != ' ')
	  && EntryEnd < End)
      EntryEnd++;
   if (EntryEnd == End)
      return false;

   Hash.append(Start, EntryEnd-Start);

   /* Skip over intermediate blanks */
   Start = EntryEnd;
   while (*Start == '\t' || *Start == ' ')
      Start++;
   if (Start >= End)
      return false;
   
   EntryEnd = Start;
   /* Find the end of the second entry (the size) */
   while ((*EntryEnd != '\t' && *EntryEnd != ' ' )
	  && EntryEnd < End)
      EntryEnd++;
   if (EntryEnd == End)
      return false;
   
   Size = strtoull (Start, NULL, 10);
      
   /* Skip over intermediate blanks */
   Start = EntryEnd;
   while (*Start == '\t' || *Start == ' ')
      Start++;
   if (Start >= End)
      return false;
   
   EntryEnd = Start;
   /* Find the end of the third entry (the filename) */
   while ((*EntryEnd != '\t' && *EntryEnd != ' ' && *EntryEnd != '\n')
	  && EntryEnd < End)
      EntryEnd++;

   Name.append(Start, EntryEnd-Start);
   Start = EntryEnd; //prepare for the next round
   return true;
}
									/*}}}*/
indexRecords::indexRecords()
{
}

indexRecords::indexRecords(const string ExpectedDist) :
   ExpectedDist(ExpectedDist), ValidUntil(0)
{
}
