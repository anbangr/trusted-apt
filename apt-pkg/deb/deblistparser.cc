// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: deblistparser.cc,v 1.29.2.5 2004/01/06 01:43:44 mdz Exp $
/* ######################################################################
   
   Package Cache Generator - Generator for the cache structure.
   
   This builds the cache structure from the abstract package list parser. 
   
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include <config.h>

#include <apt-pkg/deblistparser.h>
#include <apt-pkg/error.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/aptconfiguration.h>
#include <apt-pkg/strutl.h>
#include <apt-pkg/fileutl.h>
#include <apt-pkg/crc-16.h>
#include <apt-pkg/md5.h>
#include <apt-pkg/macros.h>

#include <fnmatch.h>
#include <ctype.h>
									/*}}}*/

using std::string;

static debListParser::WordList PrioList[] = {{"important",pkgCache::State::Important},
                       {"required",pkgCache::State::Required},
                       {"standard",pkgCache::State::Standard},
                       {"optional",pkgCache::State::Optional},
	               {"extra",pkgCache::State::Extra},
                       {}};

// ListParser::debListParser - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* Provide an architecture and only this one and "all" will be accepted
   in Step(), if no Architecture is given we will accept every arch
   we would accept in general with checkArchitecture() */
debListParser::debListParser(FileFd *File, string const &Arch) : Tags(File),
				Arch(Arch) {
   if (Arch == "native")
      this->Arch = _config->Find("APT::Architecture");
   Architectures = APT::Configuration::getArchitectures();
   MultiArchEnabled = Architectures.size() > 1;
}
									/*}}}*/
// ListParser::UniqFindTagWrite - Find the tag and write a unq string	/*{{{*/
// ---------------------------------------------------------------------
/* */
unsigned long debListParser::UniqFindTagWrite(const char *Tag)
{
   const char *Start;
   const char *Stop;
   if (Section.Find(Tag,Start,Stop) == false)
      return 0;
   return WriteUniqString(Start,Stop - Start);
}
									/*}}}*/
// ListParser::Package - Return the package name			/*{{{*/
// ---------------------------------------------------------------------
/* This is to return the name of the package this section describes */
string debListParser::Package() {
   string const Result = Section.FindS("Package");
   if(unlikely(Result.empty() == true))
      _error->Error("Encountered a section with no Package: header");
   return Result;
}
									/*}}}*/
// ListParser::Architecture - Return the package arch			/*{{{*/
// ---------------------------------------------------------------------
/* This will return the Architecture of the package this section describes */
string debListParser::Architecture() {
   return Section.FindS("Architecture");
}
									/*}}}*/
// ListParser::ArchitectureAll						/*{{{*/
// ---------------------------------------------------------------------
/* */
bool debListParser::ArchitectureAll() {
   return Section.FindS("Architecture") == "all";
}
									/*}}}*/
// ListParser::Version - Return the version string			/*{{{*/
// ---------------------------------------------------------------------
/* This is to return the string describing the version in debian form,
   epoch:upstream-release. If this returns the blank string then the 
   entry is assumed to only describe package properties */
string debListParser::Version()
{
   return Section.FindS("Version");
}
									/*}}}*/
// ListParser::NewVersion - Fill in the version structure		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool debListParser::NewVersion(pkgCache::VerIterator &Ver)
{
   // Parse the section
   Ver->Section = UniqFindTagWrite("Section");

   // Parse multi-arch
   string const MultiArch = Section.FindS("Multi-Arch");
   if (MultiArch.empty() == true)
      Ver->MultiArch = pkgCache::Version::None;
   else if (MultiArch == "same") {
      // Parse multi-arch
      if (ArchitectureAll() == true)
      {
	 /* Arch all packages can't be Multi-Arch: same */
	 _error->Warning("Architecture: all package '%s' can't be Multi-Arch: same",
			Section.FindS("Package").c_str());
	 Ver->MultiArch = pkgCache::Version::None;
      }
      else
	 Ver->MultiArch = pkgCache::Version::Same;
   }
   else if (MultiArch == "foreign")
      Ver->MultiArch = pkgCache::Version::Foreign;
   else if (MultiArch == "allowed")
      Ver->MultiArch = pkgCache::Version::Allowed;
   else
   {
      _error->Warning("Unknown Multi-Arch type '%s' for package '%s'",
			MultiArch.c_str(), Section.FindS("Package").c_str());
      Ver->MultiArch = pkgCache::Version::None;
   }

   if (ArchitectureAll() == true)
      Ver->MultiArch |= pkgCache::Version::All;

   // Archive Size
   Ver->Size = Section.FindULL("Size");
   // Unpacked Size (in K)
   Ver->InstalledSize = Section.FindULL("Installed-Size");
   Ver->InstalledSize *= 1024;

   // Priority
   const char *Start;
   const char *Stop;
   if (Section.Find("Priority",Start,Stop) == true)
   {      
      if (GrabWord(string(Start,Stop-Start),PrioList,Ver->Priority) == false)
	 Ver->Priority = pkgCache::State::Extra;
   }

   if (ParseDepends(Ver,"Depends",pkgCache::Dep::Depends) == false)
      return false;
   if (ParseDepends(Ver,"Pre-Depends",pkgCache::Dep::PreDepends) == false)
      return false;
   if (ParseDepends(Ver,"Suggests",pkgCache::Dep::Suggests) == false)
      return false;
   if (ParseDepends(Ver,"Recommends",pkgCache::Dep::Recommends) == false)
      return false;
   if (ParseDepends(Ver,"Conflicts",pkgCache::Dep::Conflicts) == false)
      return false;
   if (ParseDepends(Ver,"Breaks",pkgCache::Dep::DpkgBreaks) == false)
      return false;
   if (ParseDepends(Ver,"Replaces",pkgCache::Dep::Replaces) == false)
      return false;
   if (ParseDepends(Ver,"Enhances",pkgCache::Dep::Enhances) == false)
      return false;

   // Obsolete.
   if (ParseDepends(Ver,"Optional",pkgCache::Dep::Suggests) == false)
      return false;
   
   if (ParseProvides(Ver) == false)
      return false;
   
   return true;
}
									/*}}}*/
// ListParser::Description - Return the description string		/*{{{*/
// ---------------------------------------------------------------------
/* This is to return the string describing the package in debian
   form. If this returns the blank string then the entry is assumed to
   only describe package properties */
string debListParser::Description()
{
   string const lang = DescriptionLanguage();
   if (lang.empty())
      return Section.FindS("Description");
   else
      return Section.FindS(string("Description-").append(lang).c_str());
}
                                                                        /*}}}*/
// ListParser::DescriptionLanguage - Return the description lang string	/*{{{*/
// ---------------------------------------------------------------------
/* This is to return the string describing the language of
   description. If this returns the blank string then the entry is
   assumed to describe original description. */
string debListParser::DescriptionLanguage()
{
   if (Section.FindS("Description").empty() == false)
      return "";

   std::vector<string> const lang = APT::Configuration::getLanguages(true);
   for (std::vector<string>::const_iterator l = lang.begin();
	l != lang.end(); ++l)
      if (Section.FindS(string("Description-").append(*l).c_str()).empty() == false)
	 return *l;

   return "";
}
                                                                        /*}}}*/
// ListParser::Description - Return the description_md5 MD5SumValue	/*{{{*/
// ---------------------------------------------------------------------
/* This is to return the md5 string to allow the check if it is the right
   description. If no Description-md5 is found in the section it will be
   calculated.
 */
MD5SumValue debListParser::Description_md5()
{
   string value = Section.FindS("Description-md5");

   if (value.empty()) 
   {
      MD5Summation md5;
      md5.Add((Description() + "\n").c_str());
      return md5.Result();
   } else
      return MD5SumValue(value);
}
                                                                        /*}}}*/
// ListParser::UsePackage - Update a package structure			/*{{{*/
// ---------------------------------------------------------------------
/* This is called to update the package with any new information 
   that might be found in the section */
bool debListParser::UsePackage(pkgCache::PkgIterator &Pkg,
			       pkgCache::VerIterator &Ver)
{
   if (Pkg->Section == 0)
      Pkg->Section = UniqFindTagWrite("Section");

   // Packages which are not from the "native" arch doesn't get the essential flag
   // in the default "native" mode - it is also possible to mark "all" or "none".
   // The "installed" mode is handled by ParseStatus(), See #544481 and friends.
   string const static myArch = _config->Find("APT::Architecture");
   string const static essential = _config->Find("pkgCacheGen::Essential", "native");
   if ((essential == "native" && Pkg->Arch != 0 && myArch == Pkg.Arch()) ||
       essential == "all")
      if (Section.FindFlag("Essential",Pkg->Flags,pkgCache::Flag::Essential) == false)
	 return false;
   if (Section.FindFlag("Important",Pkg->Flags,pkgCache::Flag::Important) == false)
      return false;

   if (strcmp(Pkg.Name(),"apt") == 0)
      Pkg->Flags |= pkgCache::Flag::Important;
   
   if (ParseStatus(Pkg,Ver) == false)
      return false;
   return true;
}
									/*}}}*/
// ListParser::VersionHash - Compute a unique hash for this version	/*{{{*/
// ---------------------------------------------------------------------
/* */
unsigned short debListParser::VersionHash()
{
   const char *Sections[] ={"Installed-Size",
                            "Depends",
                            "Pre-Depends",
//                            "Suggests",
//                            "Recommends",
                            "Conflicts",
                            "Breaks",
                            "Replaces",0};
   unsigned long Result = INIT_FCS;
   char S[1024];
   for (const char **I = Sections; *I != 0; I++)
   {
      const char *Start;
      const char *End;
      if (Section.Find(*I,Start,End) == false || End - Start >= (signed)sizeof(S))
	 continue;
      
      /* Strip out any spaces from the text, this undoes dpkgs reformatting
         of certain fields. dpkg also has the rather interesting notion of
         reformatting depends operators < -> <= */
      char *J = S;
      for (; Start != End; Start++)
      {
	 if (isspace(*Start) == 0)
	    *J++ = tolower_ascii(*Start);
	 if (*Start == '<' && Start[1] != '<' && Start[1] != '=')
	    *J++ = '=';
	 if (*Start == '>' && Start[1] != '>' && Start[1] != '=')
	    *J++ = '=';
      }

      Result = AddCRC16(Result,S,J - S);
   }
   
   return Result;
}
									/*}}}*/
// ListParser::ParseStatus - Parse the status field			/*{{{*/
// ---------------------------------------------------------------------
/* Status lines are of the form,
     Status: want flag status
   want = unknown, install, hold, deinstall, purge
   flag = ok, reinstreq, hold, hold-reinstreq
   status = not-installed, unpacked, half-configured,
            half-installed, config-files, post-inst-failed, 
            removal-failed, installed
   
   Some of the above are obsolete (I think?) flag = hold-* and 
   status = post-inst-failed, removal-failed at least.
 */
bool debListParser::ParseStatus(pkgCache::PkgIterator &Pkg,
				pkgCache::VerIterator &Ver)
{
   const char *Start;
   const char *Stop;
   if (Section.Find("Status",Start,Stop) == false)
      return true;

   // UsePackage() is responsible for setting the flag in the default case
   bool const static essential = _config->Find("pkgCacheGen::Essential", "") == "installed";
   if (essential == true &&
       Section.FindFlag("Essential",Pkg->Flags,pkgCache::Flag::Essential) == false)
      return false;

   // Isolate the first word
   const char *I = Start;
   for(; I < Stop && *I != ' '; I++);
   if (I >= Stop || *I != ' ')
      return _error->Error("Malformed Status line");

   // Process the want field
   WordList WantList[] = {{"unknown",pkgCache::State::Unknown},
                          {"install",pkgCache::State::Install},
                          {"hold",pkgCache::State::Hold},
                          {"deinstall",pkgCache::State::DeInstall},
                          {"purge",pkgCache::State::Purge},
                          {}};
   if (GrabWord(string(Start,I-Start),WantList,Pkg->SelectedState) == false)
      return _error->Error("Malformed 1st word in the Status line");

   // Isloate the next word
   I++;
   Start = I;
   for(; I < Stop && *I != ' '; I++);
   if (I >= Stop || *I != ' ')
      return _error->Error("Malformed status line, no 2nd word");

   // Process the flag field
   WordList FlagList[] = {{"ok",pkgCache::State::Ok},
                          {"reinstreq",pkgCache::State::ReInstReq},
                          {"hold",pkgCache::State::HoldInst},
                          {"hold-reinstreq",pkgCache::State::HoldReInstReq},
                          {}};
   if (GrabWord(string(Start,I-Start),FlagList,Pkg->InstState) == false)
      return _error->Error("Malformed 2nd word in the Status line");

   // Isloate the last word
   I++;
   Start = I;
   for(; I < Stop && *I != ' '; I++);
   if (I != Stop)
      return _error->Error("Malformed Status line, no 3rd word");

   // Process the flag field
   WordList StatusList[] = {{"not-installed",pkgCache::State::NotInstalled},
                            {"unpacked",pkgCache::State::UnPacked},
                            {"half-configured",pkgCache::State::HalfConfigured},
                            {"installed",pkgCache::State::Installed},
                            {"half-installed",pkgCache::State::HalfInstalled},
                            {"config-files",pkgCache::State::ConfigFiles},
                            {"triggers-awaited",pkgCache::State::TriggersAwaited},
                            {"triggers-pending",pkgCache::State::TriggersPending},
                            {"post-inst-failed",pkgCache::State::HalfConfigured},
                            {"removal-failed",pkgCache::State::HalfInstalled},
                            {}};
   if (GrabWord(string(Start,I-Start),StatusList,Pkg->CurrentState) == false)
      return _error->Error("Malformed 3rd word in the Status line");

   /* A Status line marks the package as indicating the current
      version as well. Only if it is actually installed.. Otherwise
      the interesting dpkg handling of the status file creates bogus 
      entries. */
   if (!(Pkg->CurrentState == pkgCache::State::NotInstalled ||
	 Pkg->CurrentState == pkgCache::State::ConfigFiles))
   {
      if (Ver.end() == true)
	 _error->Warning("Encountered status field in a non-version description");
      else
	 Pkg->CurrentVer = Ver.Index();
   }
   
   return true;
}

const char *debListParser::ConvertRelation(const char *I,unsigned int &Op)
{
   // Determine the operator
   switch (*I)
   {
      case '<':
      I++;
      if (*I == '=')
      {
	 I++;
	 Op = pkgCache::Dep::LessEq;
	 break;
      }
      
      if (*I == '<')
      {
	 I++;
	 Op = pkgCache::Dep::Less;
	 break;
      }
      
      // < is the same as <= and << is really Cs < for some reason
      Op = pkgCache::Dep::LessEq;
      break;
      
      case '>':
      I++;
      if (*I == '=')
      {
	 I++;
	 Op = pkgCache::Dep::GreaterEq;
	 break;
      }
      
      if (*I == '>')
      {
	 I++;
	 Op = pkgCache::Dep::Greater;
	 break;
      }
      
      // > is the same as >= and >> is really Cs > for some reason
      Op = pkgCache::Dep::GreaterEq;
      break;
      
      case '=':
      Op = pkgCache::Dep::Equals;
      I++;
      break;
      
      // HACK around bad package definitions
      default:
      Op = pkgCache::Dep::Equals;
      break;
   }
   return I;
}

/*
 * CompleteArch:
 *
 * The complete architecture, consisting of <kernel>-<cpu>.
 */
static string CompleteArch(std::string const &arch) {
    if (arch == "armel")              return "linux-arm";
    if (arch == "armhf")              return "linux-arm";
    if (arch == "lpia")               return "linux-i386";
    if (arch == "powerpcspe")         return "linux-powerpc";
    if (arch == "uclibc-linux-armel") return "linux-arm";
    if (arch == "uclinux-armel")      return "uclinux-arm";

    return (arch.find("-") != string::npos) ? arch : "linux-" + arch;
}
									/*}}}*/
// ListParser::ParseDepends - Parse a dependency element		/*{{{*/
// ---------------------------------------------------------------------
/* This parses the dependency elements out of a standard string in place,
   bit by bit. */
const char *debListParser::ParseDepends(const char *Start,const char *Stop,
					string &Package,string &Ver,
					unsigned int &Op, bool const &ParseArchFlags,
					bool const &StripMultiArch)
{
   // Strip off leading space
   for (;Start != Stop && isspace(*Start) != 0; Start++);
   
   // Parse off the package name
   const char *I = Start;
   for (;I != Stop && isspace(*I) == 0 && *I != '(' && *I != ')' &&
	*I != ',' && *I != '|' && *I != '[' && *I != ']'; I++);
   
   // Malformed, no '('
   if (I != Stop && *I == ')')
      return 0;

   if (I == Start)
      return 0;
   
   // Stash the package name
   Package.assign(Start,I - Start);

   // We don't want to confuse library users which can't handle MultiArch
   string const arch = _config->Find("APT::Architecture");
   if (StripMultiArch == true) {
      size_t const found = Package.rfind(':');
      if (found != string::npos &&
	  (strcmp(Package.c_str() + found, ":any") == 0 ||
	   strcmp(Package.c_str() + found, ":native") == 0 ||
	   strcmp(Package.c_str() + found + 1, arch.c_str()) == 0))
	 Package = Package.substr(0,found);
   }

   // Skip white space to the '('
   for (;I != Stop && isspace(*I) != 0 ; I++);
   
   // Parse a version
   if (I != Stop && *I == '(')
   {
      // Skip the '('
      for (I++; I != Stop && isspace(*I) != 0 ; I++);
      if (I + 3 >= Stop)
	 return 0;
      I = ConvertRelation(I,Op);
      
      // Skip whitespace
      for (;I != Stop && isspace(*I) != 0; I++);
      Start = I;
      I = (const char*) memchr(I, ')', Stop - I);
      if (I == NULL || Start == I)
	 return 0;
      
      // Skip trailing whitespace
      const char *End = I;
      for (; End > Start && isspace(End[-1]); End--);
      
      Ver.assign(Start,End-Start);
      I++;
   }
   else
   {
      Ver.clear();
      Op = pkgCache::Dep::NoOp;
   }
   
   // Skip whitespace
   for (;I != Stop && isspace(*I) != 0; I++);

   if (ParseArchFlags == true)
   {
      string completeArch = CompleteArch(arch);

      // Parse an architecture
      if (I != Stop && *I == '[')
      {
	 // malformed
         I++;
         if (I == Stop)
	    return 0; 
	 
         const char *End = I;
         bool Found = false;
      	 bool NegArch = false;
         while (I != Stop) 
	 {
            // look for whitespace or ending ']'
	    while (End != Stop && !isspace(*End) && *End != ']') 
	       End++;
	 
	    if (End == Stop) 
	       return 0;

	    if (*I == '!')
            {
	       NegArch = true;
	       I++;
            }

	    if (stringcmp(arch,I,End) == 0) {
	       Found = true;
	    } else {
	       std::string wildcard = SubstVar(string(I, End), "any", "*");
	       if (fnmatch(wildcard.c_str(), completeArch.c_str(), 0) == 0)
	          Found = true;
	    }
	    
	    if (*End++ == ']') {
	       I = End;
	       break;
	    }
	    
	    I = End;
	    for (;I != Stop && isspace(*I) != 0; I++);
         }

	 if (NegArch)
	    Found = !Found;
	 
         if (Found == false)
	    Package = ""; /* not for this arch */
      }
      
      // Skip whitespace
      for (;I != Stop && isspace(*I) != 0; I++);
   }

   if (I != Stop && *I == '|')
      Op |= pkgCache::Dep::Or;
   
   if (I == Stop || *I == ',' || *I == '|')
   {
      if (I != Stop)
	 for (I++; I != Stop && isspace(*I) != 0; I++);
      return I;
   }
   
   return 0;
}
									/*}}}*/
// ListParser::ParseDepends - Parse a dependency list			/*{{{*/
// ---------------------------------------------------------------------
/* This is the higher level depends parser. It takes a tag and generates
   a complete depends tree for the given version. */
bool debListParser::ParseDepends(pkgCache::VerIterator &Ver,
				 const char *Tag,unsigned int Type)
{
   const char *Start;
   const char *Stop;
   if (Section.Find(Tag,Start,Stop) == false)
      return true;

   string Package;
   string const pkgArch = Ver.Arch();
   string Version;
   unsigned int Op;

   while (1)
   {
      Start = ParseDepends(Start,Stop,Package,Version,Op,false,!MultiArchEnabled);
      if (Start == 0)
	 return _error->Error("Problem parsing dependency %s",Tag);

      if (MultiArchEnabled == true &&
	  (Type == pkgCache::Dep::Conflicts ||
	   Type == pkgCache::Dep::DpkgBreaks ||
	   Type == pkgCache::Dep::Replaces))
      {
	 for (std::vector<std::string>::const_iterator a = Architectures.begin();
	      a != Architectures.end(); ++a)
	    if (NewDepends(Ver,Package,*a,Version,Op,Type) == false)
	       return false;
      }
      else if (NewDepends(Ver,Package,pkgArch,Version,Op,Type) == false)
	 return false;
      if (Start == Stop)
	 break;
   }
   return true;
}
									/*}}}*/
// ListParser::ParseProvides - Parse the provides list			/*{{{*/
// ---------------------------------------------------------------------
/* */
bool debListParser::ParseProvides(pkgCache::VerIterator &Ver)
{
   const char *Start;
   const char *Stop;
   if (Section.Find("Provides",Start,Stop) == true)
   {
      string Package;
      string Version;
      string const Arch = Ver.Arch();
      unsigned int Op;

      while (1)
      {
	 Start = ParseDepends(Start,Stop,Package,Version,Op);
	 if (Start == 0)
	    return _error->Error("Problem parsing Provides line");
	 if (Op != pkgCache::Dep::NoOp) {
	    _error->Warning("Ignoring Provides line with DepCompareOp for package %s", Package.c_str());
	 } else if ((Ver->MultiArch & pkgCache::Version::Foreign) == pkgCache::Version::Foreign) {
	    if (NewProvidesAllArch(Ver, Package, Version) == false)
	       return false;
	 } else {
	    if (NewProvides(Ver, Package, Arch, Version) == false)
	       return false;
	 }

	 if (Start == Stop)
	    break;
      }
   }

   if (MultiArchEnabled == false)
      return true;
   else if ((Ver->MultiArch & pkgCache::Version::Allowed) == pkgCache::Version::Allowed)
   {
      string const Package = string(Ver.ParentPkg().Name()).append(":").append("any");
      return NewProvidesAllArch(Ver, Package, Ver.VerStr());
   }
   else if ((Ver->MultiArch & pkgCache::Version::Foreign) == pkgCache::Version::Foreign)
      return NewProvidesAllArch(Ver, Ver.ParentPkg().Name(), Ver.VerStr());

   return true;
}
									/*}}}*/
// ListParser::NewProvides - add provides for all architectures		/*{{{*/
bool debListParser::NewProvidesAllArch(pkgCache::VerIterator &Ver, string const &Package,
				string const &Version) {
   for (std::vector<string>::const_iterator a = Architectures.begin();
	a != Architectures.end(); ++a)
   {
      if (NewProvides(Ver, Package, *a, Version) == false)
	 return false;
   }
   return true;
}
									/*}}}*/
// ListParser::GrabWord - Matches a word and returns			/*{{{*/
// ---------------------------------------------------------------------
/* Looks for a word in a list of words - for ParseStatus */
bool debListParser::GrabWord(string Word,WordList *List,unsigned char &Out)
{
   for (unsigned int C = 0; List[C].Str != 0; C++)
   {
      if (strcasecmp(Word.c_str(),List[C].Str) == 0)
      {
	 Out = List[C].Val;
	 return true;
      }
   }
   return false;
}
									/*}}}*/
// ListParser::Step - Move to the next section in the file		/*{{{*/
// ---------------------------------------------------------------------
/* This has to be carefull to only process the correct architecture */
bool debListParser::Step()
{
   iOffset = Tags.Offset();
   while (Tags.Step(Section) == true)
   {      
      /* See if this is the correct Architecture, if it isn't then we
         drop the whole section. A missing arch tag only happens (in theory)
         inside the Status file, so that is a positive return */
      string const Architecture = Section.FindS("Architecture");
      if (Architecture.empty() == true)
	 return true;

      if (Arch.empty() == true || Arch == "any" || MultiArchEnabled == false)
      {
	 if (APT::Configuration::checkArchitecture(Architecture) == true)
	    return true;
      }
      else
      {
	 if (Architecture == Arch)
	    return true;

	 if (Architecture == "all" && Arch == _config->Find("APT::Architecture"))
	    return true;
      }

      iOffset = Tags.Offset();
   }   
   return false;
}
									/*}}}*/
// ListParser::LoadReleaseInfo - Load the release information		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool debListParser::LoadReleaseInfo(pkgCache::PkgFileIterator &FileI,
				    FileFd &File, string component)
{
   // apt-secure does no longer download individual (per-section) Release
   // file. to provide Component pinning we use the section name now
   FileI->Component = WriteUniqString(component);

   // FIXME: Code depends on the fact that Release files aren't compressed
   FILE* release = fdopen(dup(File.Fd()), "r");
   if (release == NULL)
      return false;

   char buffer[101];
   bool gpgClose = false;
   while (fgets(buffer, sizeof(buffer), release) != NULL)
   {
      size_t len = 0;

      // Skip empty lines
      for (; buffer[len] == '\r' && buffer[len] == '\n'; ++len)
         /* nothing */
         ;
      if (buffer[len] == '\0')
	 continue;

      // only evalute the first GPG section
      if (strncmp("-----", buffer, 5) == 0)
      {
	 if (gpgClose == true)
	    break;
	 gpgClose = true;
	 continue;
      }

      // seperate the tag from the data
      const char* dataStart = strchr(buffer + len, ':');
      if (dataStart == NULL)
	 continue;
      len = dataStart - buffer;
      for (++dataStart; *dataStart == ' '; ++dataStart)
         /* nothing */
         ;
      const char* dataEnd = (const char*)rawmemchr(dataStart, '\0');
      // The last char should be a newline, but we can never be sure: #633350
      const char* lineEnd = dataEnd;
      for (--lineEnd; *lineEnd == '\r' || *lineEnd == '\n'; --lineEnd)
         /* nothing */
         ;
      ++lineEnd;

      // which datastorage need to be updated
      enum { Suite, Component, Version, Origin, Codename, Label, None } writeTo = None;
      if (buffer[0] == ' ')
	 ;
      #define APT_PARSER_WRITETO(X) else if (strncmp(#X, buffer, len) == 0) writeTo = X;
      APT_PARSER_WRITETO(Suite)
      APT_PARSER_WRITETO(Component)
      APT_PARSER_WRITETO(Version)
      APT_PARSER_WRITETO(Origin)
      APT_PARSER_WRITETO(Codename)
      APT_PARSER_WRITETO(Label)
      #undef APT_PARSER_WRITETO
      #define APT_PARSER_FLAGIT(X) else if (strncmp(#X, buffer, len) == 0) \
	 pkgTagSection::FindFlag(FileI->Flags, pkgCache::Flag:: X, dataStart, lineEnd);
      APT_PARSER_FLAGIT(NotAutomatic)
      APT_PARSER_FLAGIT(ButAutomaticUpgrades)
      #undef APT_PARSER_FLAGIT

      // load all data from the line and save it
      string data;
      if (writeTo != None)
	 data.append(dataStart, dataEnd);
      if (sizeof(buffer) - 1 == (dataEnd - buffer))
      {
	 while (fgets(buffer, sizeof(buffer), release) != NULL)
	 {
	    if (writeTo != None)
	       data.append(buffer);
	    if (strlen(buffer) != sizeof(buffer) - 1)
	       break;
	 }
      }
      if (writeTo != None)
      {
	 // remove spaces and stuff from the end of the data line
	 for (std::string::reverse_iterator s = data.rbegin();
	      s != data.rend(); ++s)
	 {
	    if (*s != '\r' && *s != '\n' && *s != ' ')
	       break;
	    *s = '\0';
	 }
	 switch (writeTo) {
	 case Suite: FileI->Archive = WriteUniqString(data); break;
	 case Component: FileI->Component = WriteUniqString(data); break;
	 case Version: FileI->Version = WriteUniqString(data); break;
	 case Origin: FileI->Origin = WriteUniqString(data); break;
	 case Codename: FileI->Codename = WriteUniqString(data); break;
	 case Label: FileI->Label = WriteUniqString(data); break;
	 case None: break;
	 }
      }
   }
   fclose(release);

   return !_error->PendingError();
}
									/*}}}*/
// ListParser::GetPrio - Convert the priority from a string		/*{{{*/
// ---------------------------------------------------------------------
/* */
unsigned char debListParser::GetPrio(string Str)
{
   unsigned char Out;
   if (GrabWord(Str,PrioList,Out) == false)
      Out = pkgCache::State::Extra;
   
   return Out;
}
									/*}}}*/
