// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: packagemanager.cc,v 1.30 2003/04/27 03:04:15 doogie Exp $
/* ######################################################################

   Package Manager - Abstacts the package manager

   More work is needed in the area of transitioning provides, ie exim
   replacing smail. This can cause interesing side effects.

   Other cases involving conflicts+replaces should be tested. 
   
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include<config.h>

#include <apt-pkg/packagemanager.h>
#include <apt-pkg/orderlist.h>
#include <apt-pkg/depcache.h>
#include <apt-pkg/error.h>
#include <apt-pkg/version.h>
#include <apt-pkg/acquire-item.h>
#include <apt-pkg/algorithms.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/sptr.h>

#include <apti18n.h>
#include <iostream>
#include <fcntl.h>
									/*}}}*/
using namespace std;

bool pkgPackageManager::SigINTStop = false;

// PM::PackageManager - Constructor					/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgPackageManager::pkgPackageManager(pkgDepCache *pCache) : Cache(*pCache),
							    List(NULL), Res(Incomplete)
{
   FileNames = new string[Cache.Head().PackageCount];
   Debug = _config->FindB("Debug::pkgPackageManager",false);
   NoImmConfigure = !_config->FindB("APT::Immediate-Configure",true);
   ImmConfigureAll = _config->FindB("APT::Immediate-Configure-All",false);
}
									/*}}}*/
// PM::PackageManager - Destructor					/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgPackageManager::~pkgPackageManager()
{
   delete List;
   delete [] FileNames;
}
									/*}}}*/
// PM::GetArchives - Queue the archives for download			/*{{{*/
// ---------------------------------------------------------------------
/* */
bool pkgPackageManager::GetArchives(pkgAcquire *Owner,pkgSourceList *Sources,
				    pkgRecords *Recs)
{
   if (CreateOrderList() == false)
      return false;
   
   bool const ordering =
	_config->FindB("PackageManager::UnpackAll",true) ?
		List->OrderUnpack() : List->OrderCritical();
   if (ordering == false)
      return _error->Error("Internal ordering error");

   for (pkgOrderList::iterator I = List->begin(); I != List->end(); ++I)
   {
      PkgIterator Pkg(Cache,*I);
      FileNames[Pkg->ID] = string();
      
      // Skip packages to erase
      if (Cache[Pkg].Delete() == true)
	 continue;

      // Skip Packages that need configure only.
      if (Pkg.State() == pkgCache::PkgIterator::NeedsConfigure && 
	  Cache[Pkg].Keep() == true)
	 continue;

      // Skip already processed packages
      if (List->IsNow(Pkg) == false)
	 continue;

      new pkgAcqArchive(Owner,Sources,Recs,Cache[Pkg].InstVerIter(Cache),
			FileNames[Pkg->ID]);
   }

   return true;
}
									/*}}}*/
// PM::FixMissing - Keep all missing packages				/*{{{*/
// ---------------------------------------------------------------------
/* This is called to correct the installation when packages could not
   be downloaded. */
bool pkgPackageManager::FixMissing()
{   
   pkgDepCache::ActionGroup group(Cache);
   pkgProblemResolver Resolve(&Cache);
   List->SetFileList(FileNames);

   bool Bad = false;
   for (PkgIterator I = Cache.PkgBegin(); I.end() == false; ++I)
   {
      if (List->IsMissing(I) == false)
	 continue;
   
      // Okay, this file is missing and we need it. Mark it for keep 
      Bad = true;
      Cache.MarkKeep(I, false, false);
   }
 
   // We have to empty the list otherwise it will not have the new changes
   delete List;
   List = 0;
   
   if (Bad == false)
      return true;
   
   // Now downgrade everything that is broken
   return Resolve.ResolveByKeep() == true && Cache.BrokenCount() == 0;   
}
									/*}}}*/
// PM::ImmediateAdd - Add the immediate flag recursivly			/*{{{*/
// ---------------------------------------------------------------------
/* This adds the immediate flag to the pkg and recursively to the
   dependendies 
 */
void pkgPackageManager::ImmediateAdd(PkgIterator I, bool UseInstallVer, unsigned const int &Depth)
{
   DepIterator D;
   
   if(UseInstallVer)
   {
      if(Cache[I].InstallVer == 0)
	 return;
      D = Cache[I].InstVerIter(Cache).DependsList(); 
   } else {
      if (I->CurrentVer == 0)
	 return;
      D = I.CurrentVer().DependsList(); 
   }

   for ( /* nothing */  ; D.end() == false; ++D)
      if (D->Type == pkgCache::Dep::Depends || D->Type == pkgCache::Dep::PreDepends)
      {
	 if(!List->IsFlag(D.TargetPkg(), pkgOrderList::Immediate))
	 {
	    if(Debug)
	       clog << OutputInDepth(Depth) << "ImmediateAdd(): Adding Immediate flag to " << D.TargetPkg() << " cause of " << D.DepType() << " " << I.FullName() << endl;
	    List->Flag(D.TargetPkg(),pkgOrderList::Immediate);
	    ImmediateAdd(D.TargetPkg(), UseInstallVer, Depth + 1);
	 }
      }
   return;
}
									/*}}}*/
// PM::CreateOrderList - Create the ordering class			/*{{{*/
// ---------------------------------------------------------------------
/* This populates the ordering list with all the packages that are
   going to change. */
bool pkgPackageManager::CreateOrderList()
{
   if (List != 0)
      return true;
   
   delete List;
   List = new pkgOrderList(&Cache);

   if (Debug && ImmConfigureAll) 
      clog << "CreateOrderList(): Adding Immediate flag for all packages because of APT::Immediate-Configure-All" << endl;
   
   // Generate the list of affected packages and sort it
   for (PkgIterator I = Cache.PkgBegin(); I.end() == false; ++I)
   {
      // Ignore no-version packages
      if (I->VersionList == 0)
	 continue;
      
      // Mark the package and its dependends for immediate configuration
      if ((((I->Flags & pkgCache::Flag::Essential) == pkgCache::Flag::Essential ||
	   (I->Flags & pkgCache::Flag::Important) == pkgCache::Flag::Important) &&
	  NoImmConfigure == false) || ImmConfigureAll)
      {
	 if(Debug && !ImmConfigureAll)
	    clog << "CreateOrderList(): Adding Immediate flag for " << I.FullName() << endl;
	 List->Flag(I,pkgOrderList::Immediate);
	 
	 if (!ImmConfigureAll) {
	    // Look for other install packages to make immediate configurea
	    ImmediateAdd(I, true);
	  
	    // And again with the current version.
	    ImmediateAdd(I, false);
	 }
      }
      
      // Not interesting
      if ((Cache[I].Keep() == true || 
	  Cache[I].InstVerIter(Cache) == I.CurrentVer()) && 
	  I.State() == pkgCache::PkgIterator::NeedsNothing &&
	  (Cache[I].iFlags & pkgDepCache::ReInstall) != pkgDepCache::ReInstall &&
	  (I.Purge() != false || Cache[I].Mode != pkgDepCache::ModeDelete ||
	   (Cache[I].iFlags & pkgDepCache::Purge) != pkgDepCache::Purge))
	 continue;
      
      // Append it to the list
      List->push_back(I);      
   }
   
   return true;
}
									/*}}}*/
// PM::DepAlwaysTrue - Returns true if this dep is irrelevent		/*{{{*/
// ---------------------------------------------------------------------
/* The restriction on provides is to eliminate the case when provides
   are transitioning between valid states [ie exim to smail] */
bool pkgPackageManager::DepAlwaysTrue(DepIterator D)
{
   if (D.TargetPkg()->ProvidesList != 0)
      return false;
   
   if ((Cache[D] & pkgDepCache::DepInstall) != 0 &&
       (Cache[D] & pkgDepCache::DepNow) != 0)
      return true;
   return false;
}
									/*}}}*/
// PM::CheckRConflicts - Look for reverse conflicts			/*{{{*/
// ---------------------------------------------------------------------
/* This looks over the reverses for a conflicts line that needs early
   removal. */
bool pkgPackageManager::CheckRConflicts(PkgIterator Pkg,DepIterator D,
					const char *Ver)
{
   for (;D.end() == false; ++D)
   {
      if (D->Type != pkgCache::Dep::Conflicts &&
	  D->Type != pkgCache::Dep::Obsoletes)
	 continue;

      // The package hasnt been changed
      if (List->IsNow(Pkg) == false)
	 continue;
      
      // Ignore self conflicts, ignore conflicts from irrelevent versions
      if (D.IsIgnorable(Pkg) || D.ParentVer() != D.ParentPkg().CurrentVer())
	 continue;
      
      if (Cache.VS().CheckDep(Ver,D->CompareOp,D.TargetVer()) == false)
	 continue;

      if (EarlyRemove(D.ParentPkg()) == false)
	 return _error->Error("Reverse conflicts early remove for package '%s' failed",
			      Pkg.FullName().c_str());
   }
   return true;
}
									/*}}}*/
// PM::ConfigureAll - Run the all out configuration			/*{{{*/
// ---------------------------------------------------------------------
/* This configures every package. It is assumed they are all unpacked and
   that the final configuration is valid. This is also used to catch packages
   that have not been configured when using ImmConfigureAll */
bool pkgPackageManager::ConfigureAll()
{
   pkgOrderList OList(&Cache);
   
   // Populate the order list
   for (pkgOrderList::iterator I = List->begin(); I != List->end(); ++I)
      if (List->IsFlag(pkgCache::PkgIterator(Cache,*I),
		       pkgOrderList::UnPacked) == true)
	 OList.push_back(*I);
   
   if (OList.OrderConfigure() == false)
      return false;

   std::string const conf = _config->Find("PackageManager::Configure","all");
   bool const ConfigurePkgs = (conf == "all");

   // Perform the configuring
   for (pkgOrderList::iterator I = OList.begin(); I != OList.end(); ++I)
   {
      PkgIterator Pkg(Cache,*I);
      
      /* Check if the package has been configured, this can happen if SmartConfigure
         calls its self */ 
      if (List->IsFlag(Pkg,pkgOrderList::Configured)) continue;

      if (ConfigurePkgs == true && SmartConfigure(Pkg, 0) == false) {
         if (ImmConfigureAll)
            _error->Error(_("Could not perform immediate configuration on '%s'. "
			"Please see man 5 apt.conf under APT::Immediate-Configure for details. (%d)"),Pkg.FullName().c_str(),1);
         else
            _error->Error("Internal error, packages left unconfigured. %s",Pkg.FullName().c_str());
	 return false;
      }
      
      List->Flag(Pkg,pkgOrderList::Configured,pkgOrderList::States);
   }
   
   return true;
}
									/*}}}*/
// PM::SmartConfigure - Perform immediate configuration of the pkg	/*{{{*/
// ---------------------------------------------------------------------
/* This function tries to put the system in a state where Pkg can be configured.
   This involves checking each of Pkg's dependanies and unpacking and 
   configuring packages where needed. 
   
   Note on failure: This method can fail, without causing any problems. 
   This can happen when using Immediate-Configure-All, SmartUnPack may call
   SmartConfigure, it may fail because of a complex dependancy situation, but
   a error will only be reported if ConfigureAll fails. This is why some of the
   messages this function reports on failure (return false;) as just warnings
   only shown when debuging*/
bool pkgPackageManager::SmartConfigure(PkgIterator Pkg, int const Depth)
{
   // If this is true, only check and correct and dependencies without the Loop flag
   bool const PkgLoop = List->IsFlag(Pkg,pkgOrderList::Loop);

   if (Debug) {
      VerIterator InstallVer = VerIterator(Cache,Cache[Pkg].InstallVer);
      clog << OutputInDepth(Depth) << "SmartConfigure " << Pkg.FullName() << " (" << InstallVer.VerStr() << ")";
      if (PkgLoop)
        clog << " (Only Correct Dependencies)";
      clog << endl;
   }

   VerIterator const instVer = Cache[Pkg].InstVerIter(Cache);
      
   /* Because of the ordered list, most dependencies should be unpacked, 
      however if there is a loop (A depends on B, B depends on A) this will not 
      be the case, so check for dependencies before configuring. */
   bool Bad = false, Changed = false;
   const unsigned int max_loops = _config->FindI("APT::pkgPackageManager::MaxLoopCount", 5000);
   unsigned int i=0;
   do
   {
      Changed = false;
      for (DepIterator D = instVer.DependsList(); D.end() == false; )
      {
	 // Compute a single dependency element (glob or)
	 pkgCache::DepIterator Start, End;
	 D.GlobOr(Start,End);

	 if (End->Type != pkgCache::Dep::Depends)
	    continue;
	 Bad = true;

	 // Search for dependencies which are unpacked but aren't configured yet (maybe loops)
	 for (DepIterator Cur = Start; true; ++Cur)
	 {
	    SPtrArray<Version *> VList = Cur.AllTargets();

	    for (Version **I = VList; *I != 0; ++I)
	    {
	       VerIterator Ver(Cache,*I);
	       PkgIterator DepPkg = Ver.ParentPkg();

	       // Check if the current version of the package is available and will satisfy this dependency
	       if (DepPkg.CurrentVer() == Ver && List->IsNow(DepPkg) == true &&
		   List->IsFlag(DepPkg,pkgOrderList::Removed) == false &&
		   DepPkg.State() == PkgIterator::NeedsNothing)
	       {
		  Bad = false;
		  break;
	       }

	       // Check if the version that is going to be installed will satisfy the dependency
	       if (Cache[DepPkg].InstallVer != *I)
		  continue;

	       if (List->IsFlag(DepPkg,pkgOrderList::UnPacked))
	       {
		  if (List->IsFlag(DepPkg,pkgOrderList::Loop) && PkgLoop)
		  {
		    // This dependency has already been dealt with by another SmartConfigure on Pkg
		    Bad = false;
		    break;
		  }
		  /* Check for a loop to prevent one forming
		       If A depends on B and B depends on A, SmartConfigure will
		       just hop between them if this is not checked. Dont remove the
		       loop flag after finishing however as loop is already set.
		       This means that there is another SmartConfigure call for this
		       package and it will remove the loop flag */
		  if (PkgLoop == false)
		     List->Flag(Pkg,pkgOrderList::Loop);
		  if (SmartConfigure(DepPkg, Depth + 1) == true)
		  {
		     Bad = false;
		     if (List->IsFlag(DepPkg,pkgOrderList::Loop) == false)
			Changed = true;
		  }
		  if (PkgLoop == false)
		    List->RmFlag(Pkg,pkgOrderList::Loop);
		  // If SmartConfigure was succesfull, Bad is false, so break
		  if (Bad == false)
		     break;
	       }
	       else if (List->IsFlag(DepPkg,pkgOrderList::Configured))
	       {
		  Bad = false;
		  break;
	       }
	    }
	    if (Cur == End)
	       break;
         }

	 if (Bad == false)
	    continue;

	 // Check for dependencies that have not been unpacked, probably due to loops.
	 for (DepIterator Cur = Start; true; ++Cur)
	 {
	    SPtrArray<Version *> VList = Cur.AllTargets();

	    for (Version **I = VList; *I != 0; ++I)
	    {
	       VerIterator Ver(Cache,*I);
	       PkgIterator DepPkg = Ver.ParentPkg();

	       // Check if the version that is going to be installed will satisfy the dependency
	       if (Cache[DepPkg].InstallVer != *I || List->IsNow(DepPkg) == false)
		  continue;

	       if (PkgLoop == true)
	       {
		  if (Debug)
		     std::clog << OutputInDepth(Depth) << "Package " << Pkg << " loops in SmartConfigure" << std::endl;
	          Bad = false;
		  break;
	       }
	       else
	       {
		  if (Debug)
		     clog << OutputInDepth(Depth) << "Unpacking " << DepPkg.FullName() << " to avoid loop " << Cur << endl;
		  if (PkgLoop == false)
		     List->Flag(Pkg,pkgOrderList::Loop);
		  if (SmartUnPack(DepPkg, true, Depth + 1) == true)
		  {
		     Bad = false;
		     if (List->IsFlag(DepPkg,pkgOrderList::Loop) == false)
		        Changed = true;
		  }
		  if (PkgLoop == false)
		     List->RmFlag(Pkg,pkgOrderList::Loop);
		  if (Bad == false)
		     break;
	       }
	    }

	    if (Cur == End)
	       break;
	 }

	 if (Bad == true && Changed == false && Debug == true)
	    std::clog << OutputInDepth(Depth) << "Could not satisfy " << Start << std::endl;
      }
      if (i++ > max_loops)
         return _error->Error("Internal error: MaxLoopCount reached in SmartUnPack for %s, aborting", Pkg.FullName().c_str());
   } while (Changed == true);
   
   if (Bad) {
      if (Debug)
         _error->Warning(_("Could not configure '%s'. "),Pkg.FullName().c_str());
      return false;
   }
   
   if (PkgLoop) return true;

   static std::string const conf = _config->Find("PackageManager::Configure","all");
   static bool const ConfigurePkgs = (conf == "all" || conf == "smart");

   if (List->IsFlag(Pkg,pkgOrderList::Configured)) 
      return _error->Error("Internal configure error on '%s'.", Pkg.FullName().c_str());

   if (ConfigurePkgs == true && Configure(Pkg) == false)
      return false;

   List->Flag(Pkg,pkgOrderList::Configured,pkgOrderList::States);

   if ((Cache[Pkg].InstVerIter(Cache)->MultiArch & pkgCache::Version::Same) == pkgCache::Version::Same)
      for (PkgIterator P = Pkg.Group().PackageList();
	   P.end() == false; P = Pkg.Group().NextPkg(P))
      {
	 if (Pkg == P || List->IsFlag(P,pkgOrderList::Configured) == true ||
	     Cache[P].InstallVer == 0 || (P.CurrentVer() == Cache[P].InstallVer &&
	      (Cache[Pkg].iFlags & pkgDepCache::ReInstall) != pkgDepCache::ReInstall))
	    continue;
	 SmartConfigure(P, (Depth +1));
      }

   // Sanity Check
   if (List->IsFlag(Pkg,pkgOrderList::Configured) == false)
      return _error->Error(_("Could not configure '%s'. "),Pkg.FullName().c_str());

   return true;
}
									/*}}}*/
// PM::EarlyRemove - Perform removal of packages before their time	/*{{{*/
// ---------------------------------------------------------------------
/* This is called to deal with conflicts arising from unpacking */
bool pkgPackageManager::EarlyRemove(PkgIterator Pkg)
{
   if (List->IsNow(Pkg) == false)
      return true;
	 
   // Already removed it
   if (List->IsFlag(Pkg,pkgOrderList::Removed) == true)
      return true;
   
   // Woops, it will not be re-installed!
   if (List->IsFlag(Pkg,pkgOrderList::InList) == false)
      return false;

   // Essential packages get special treatment
   bool IsEssential = false;
   if ((Pkg->Flags & pkgCache::Flag::Essential) != 0)
      IsEssential = true;

   /* Check for packages that are the dependents of essential packages and 
      promote them too */
   if (Pkg->CurrentVer != 0)
   {
      for (DepIterator D = Pkg.RevDependsList(); D.end() == false &&
	   IsEssential == false; ++D)
	 if (D->Type == pkgCache::Dep::Depends || D->Type == pkgCache::Dep::PreDepends)
	    if ((D.ParentPkg()->Flags & pkgCache::Flag::Essential) != 0)
	       IsEssential = true;
   }

   if (IsEssential == true)
   {
      if (_config->FindB("APT::Force-LoopBreak",false) == false)
	 return _error->Error(_("This installation run will require temporarily "
				"removing the essential package %s due to a "
				"Conflicts/Pre-Depends loop. This is often bad, "
				"but if you really want to do it, activate the "
				"APT::Force-LoopBreak option."),Pkg.FullName().c_str());
   }
   
   bool Res = SmartRemove(Pkg);
   if (Cache[Pkg].Delete() == false)
      List->Flag(Pkg,pkgOrderList::Removed,pkgOrderList::States);
   
   return Res;
}
									/*}}}*/
// PM::SmartRemove - Removal Helper					/*{{{*/
// ---------------------------------------------------------------------
/* */
bool pkgPackageManager::SmartRemove(PkgIterator Pkg)
{
   if (List->IsNow(Pkg) == false)
      return true;

   List->Flag(Pkg,pkgOrderList::Configured,pkgOrderList::States);

   return Remove(Pkg,(Cache[Pkg].iFlags & pkgDepCache::Purge) == pkgDepCache::Purge);
}
									/*}}}*/
// PM::SmartUnPack - Install helper					/*{{{*/
// ---------------------------------------------------------------------
/* This puts the system in a state where it can Unpack Pkg, if Pkg is allready
   unpacked, or when it has been unpacked, if Immediate==true it configures it. */
bool pkgPackageManager::SmartUnPack(PkgIterator Pkg)
{
   return SmartUnPack(Pkg, true, 0);
}
bool pkgPackageManager::SmartUnPack(PkgIterator Pkg, bool const Immediate, int const Depth)
{
   bool PkgLoop = List->IsFlag(Pkg,pkgOrderList::Loop);

   if (Debug) {
      clog << OutputInDepth(Depth) << "SmartUnPack " << Pkg.FullName();
      VerIterator InstallVer = VerIterator(Cache,Cache[Pkg].InstallVer);
      if (Pkg.CurrentVer() == 0)
        clog << " (install version " << InstallVer.VerStr() << ")";
      else
        clog << " (replace version " << Pkg.CurrentVer().VerStr() << " with " << InstallVer.VerStr() << ")";
      if (PkgLoop)
        clog << " (Only Perform PreUnpack Checks)";
      clog << endl;
   }

   VerIterator const instVer = Cache[Pkg].InstVerIter(Cache);

   /* PreUnpack Checks: This loop checks and attempts to rectify and problems that would prevent the package being unpacked.
      It addresses: PreDepends, Conflicts, Obsoletes and Breaks (DpkgBreaks). Any resolutions that do not require it should
      avoid configuration (calling SmartUnpack with Immediate=true), this is because when unpacking some packages with
      complex dependancy structures, trying to configure some packages while breaking the loops can complicate things .
      This will be either dealt with if the package is configured as a dependency of Pkg (if and when Pkg is configured),
      or by the ConfigureAll call at the end of the for loop in OrderInstall. */
   bool Changed = false;
   const unsigned int max_loops = _config->FindI("APT::pkgPackageManager::MaxLoopCount", 5000);
   unsigned int i=0;
   do 
   {
      Changed = false;
      for (DepIterator D = instVer.DependsList(); D.end() == false; )
      {
	 // Compute a single dependency element (glob or)
	 pkgCache::DepIterator Start, End;
	 D.GlobOr(Start,End);

	 if (End->Type == pkgCache::Dep::PreDepends)
         {
	    bool Bad = true;
	    if (Debug)
	       clog << OutputInDepth(Depth) << "PreDepends order for " << Pkg.FullName() << std::endl;

	    // Look for easy targets: packages that are already okay
	    for (DepIterator Cur = Start; Bad == true; ++Cur)
	    {
	       SPtrArray<Version *> VList = Cur.AllTargets();
	       for (Version **I = VList; *I != 0; ++I)
	       {
		  VerIterator Ver(Cache,*I);
		  PkgIterator Pkg = Ver.ParentPkg();

		  // See if the current version is ok
		  if (Pkg.CurrentVer() == Ver && List->IsNow(Pkg) == true &&
		      Pkg.State() == PkgIterator::NeedsNothing)
		  {
		     Bad = false;
		     if (Debug)
			clog << OutputInDepth(Depth) << "Found ok package " << Pkg.FullName() << endl;
		     break;
		  }
	       }
	       if (Cur == End)
		  break;
	    }

	    // Look for something that could be configured.
	    for (DepIterator Cur = Start; Bad == true; ++Cur)
	    {
	       SPtrArray<Version *> VList = Cur.AllTargets();
	       for (Version **I = VList; *I != 0; ++I)
	       {
		  VerIterator Ver(Cache,*I);
		  PkgIterator Pkg = Ver.ParentPkg();

		  // Not the install version
		  if (Cache[Pkg].InstallVer != *I ||
		      (Cache[Pkg].Keep() == true && Pkg.State() == PkgIterator::NeedsNothing))
		     continue;

		  if (List->IsFlag(Pkg,pkgOrderList::Configured))
		  {
		     Bad = false;
		     break;
		  }

		  // check if it needs unpack or if if configure is enough
		  if (List->IsFlag(Pkg,pkgOrderList::UnPacked) == false)
		  {
		     if (Debug)
			clog << OutputInDepth(Depth) << "Trying to SmartUnpack " << Pkg.FullName() << endl;
		     // SmartUnpack with the ImmediateFlag to ensure its really ready
		     if (SmartUnPack(Pkg, true, Depth + 1) == true)
		     {
			Bad = false;
			if (List->IsFlag(Pkg,pkgOrderList::Loop) == false)
			   Changed = true;
			break;
		     }
		  }
		  else
		  {
		     if (Debug)
			clog << OutputInDepth(Depth) << "Trying to SmartConfigure " << Pkg.FullName() << endl;
		     if (SmartConfigure(Pkg, Depth + 1) == true)
		     {
			Bad = false;
			if (List->IsFlag(Pkg,pkgOrderList::Loop) == false)
			   Changed = true;
			break;
		     }
		  }
	       }
	    }

	    if (Bad == true)
	    {
	       if (Start == End)
		  return _error->Error("Couldn't configure pre-depend %s for %s, "
					"probably a dependency cycle.",
					End.TargetPkg().FullName().c_str(),Pkg.FullName().c_str());
	    }
	    else
	       continue;
	 }
	 else if (End->Type == pkgCache::Dep::Conflicts ||
		  End->Type == pkgCache::Dep::Obsoletes)
	 {
	    /* Look for conflicts. Two packages that are both in the install
	       state cannot conflict so we don't check.. */
	    SPtrArray<Version *> VList = End.AllTargets();
	    for (Version **I = VList; *I != 0; I++)
	    {
	       VerIterator Ver(Cache,*I);
	       PkgIterator ConflictPkg = Ver.ParentPkg();
	       VerIterator InstallVer(Cache,Cache[ConflictPkg].InstallVer);

	       // See if the current version is conflicting
	       if (ConflictPkg.CurrentVer() == Ver && List->IsNow(ConflictPkg))
	       {
		  clog << OutputInDepth(Depth) << Pkg.FullName() << " conflicts with " << ConflictPkg.FullName() << endl;
		  /* If a loop is not present or has not yet been detected, attempt to unpack packages
		     to resolve this conflict. If there is a loop present, remove packages to resolve this conflict */
		  if (List->IsFlag(ConflictPkg,pkgOrderList::Loop) == false)
		  {
		     if (Cache[ConflictPkg].Keep() == 0 && Cache[ConflictPkg].InstallVer != 0)
		     {
			if (Debug)
			   clog << OutputInDepth(Depth) << OutputInDepth(Depth) << "Unpacking " << ConflictPkg.FullName() << " to prevent conflict" << endl;
			List->Flag(Pkg,pkgOrderList::Loop);
			if (SmartUnPack(ConflictPkg,false, Depth + 1) == true)
			   if (List->IsFlag(ConflictPkg,pkgOrderList::Loop) == false)
			      Changed = true;
			// Remove loop to allow it to be used later if needed
			List->RmFlag(Pkg,pkgOrderList::Loop);
		     }
		     else if (EarlyRemove(ConflictPkg) == false)
			return _error->Error("Internal Error, Could not early remove %s (1)",ConflictPkg.FullName().c_str());
		  }
		  else if (List->IsFlag(ConflictPkg,pkgOrderList::Removed) == false)
		  {
		     if (Debug)
			clog << OutputInDepth(Depth) << "Because of conficts knot, removing " << ConflictPkg.FullName() << " to conflict violation" << endl;
		     if (EarlyRemove(ConflictPkg) == false)
			return _error->Error("Internal Error, Could not early remove %s (2)",ConflictPkg.FullName().c_str());
		  }
	       }
	    }
	 }
	 else if (End->Type == pkgCache::Dep::DpkgBreaks)
	 {
	    SPtrArray<Version *> VList = End.AllTargets();
	    for (Version **I = VList; *I != 0; ++I)
	    {
	       VerIterator Ver(Cache,*I);
	       PkgIterator BrokenPkg = Ver.ParentPkg();
	       if (BrokenPkg.CurrentVer() != Ver)
	       {
		  if (Debug)
		     std::clog << OutputInDepth(Depth) << "  Ignore not-installed version " << Ver.VerStr() << " of " << Pkg.FullName() << " for " << End << std::endl;
		  continue;
	       }

	       // Check if it needs to be unpacked
	       if (List->IsFlag(BrokenPkg,pkgOrderList::InList) && Cache[BrokenPkg].Delete() == false &&
		   List->IsNow(BrokenPkg))
	       {
		  if (List->IsFlag(BrokenPkg,pkgOrderList::Loop) && PkgLoop)
		  {
		     // This dependancy has already been dealt with by another SmartUnPack on Pkg
		     break;
		  }
		  else
		  {
		     // Found a break, so see if we can unpack the package to avoid it
		     // but do not set loop if another SmartUnPack already deals with it
		     // Also, avoid it if the package we would unpack pre-depends on this one
		     VerIterator InstallVer(Cache,Cache[BrokenPkg].InstallVer);
		     bool circle = false;
		     for (pkgCache::DepIterator D = InstallVer.DependsList(); D.end() == false; ++D)
		     {
			if (D->Type != pkgCache::Dep::PreDepends)
			   continue;
			SPtrArray<Version *> VL = D.AllTargets();
			for (Version **I = VL; *I != 0; ++I)
			{
			   VerIterator V(Cache,*I);
			   PkgIterator P = V.ParentPkg();
			   // we are checking for installation as an easy 'protection' against or-groups and (unchosen) providers
			   if (P->CurrentVer == 0 || P != Pkg || (P.CurrentVer() != V && Cache[P].InstallVer != V))
			      continue;
			   circle = true;
			   break;
			}
			if (circle == true)
			   break;
		     }
		     if (circle == true)
		     {
			if (Debug)
			   clog << OutputInDepth(Depth) << "  Avoiding " << End << " avoided as " << BrokenPkg.FullName() << " has a pre-depends on " << Pkg.FullName() << std::endl;
			continue;
		     }
		     else
		     {
			if (Debug)
			{
			   clog << OutputInDepth(Depth) << "  Unpacking " << BrokenPkg.FullName() << " to avoid " << End;
			   if (PkgLoop == true)
			      clog << " (Looping)";
			   clog << std::endl;
			}
			if (PkgLoop == false)
			   List->Flag(Pkg,pkgOrderList::Loop);
			if (SmartUnPack(BrokenPkg, false, Depth + 1) == true)
			{
			   if (List->IsFlag(BrokenPkg,pkgOrderList::Loop) == false)
			      Changed = true;
			}
			if (PkgLoop == false)
			   List->RmFlag(Pkg,pkgOrderList::Loop);
		     }
		  }
	       }
	       // Check if a package needs to be removed
	       else if (Cache[BrokenPkg].Delete() == true && List->IsFlag(BrokenPkg,pkgOrderList::Configured) == false)
	       {
		  if (Debug)
		     clog << OutputInDepth(Depth) << "  Removing " << BrokenPkg.FullName() << " to avoid " << End << endl;
		  SmartRemove(BrokenPkg);
	       }
	    }
	 }
      }
      if (i++ > max_loops)
         return _error->Error("Internal error: APT::pkgPackageManager::MaxLoopCount reached in SmartConfigure for %s, aborting", Pkg.FullName().c_str());
   } while (Changed == true);
   
   // Check for reverse conflicts.
   if (CheckRConflicts(Pkg,Pkg.RevDependsList(),
		   instVer.VerStr()) == false)
		          return false;
   
   for (PrvIterator P = instVer.ProvidesList();
	P.end() == false; ++P)
      if (Pkg->Group != P.OwnerPkg()->Group)
	 CheckRConflicts(Pkg,P.ParentPkg().RevDependsList(),P.ProvideVersion());

   if (PkgLoop)
      return true;

   List->Flag(Pkg,pkgOrderList::UnPacked,pkgOrderList::States);

   if (Immediate == true && (instVer->MultiArch & pkgCache::Version::Same) == pkgCache::Version::Same)
   {
      /* Do lockstep M-A:same unpacking in two phases:
	 First unpack all installed architectures, then the not installed.
	 This way we avoid that M-A: enabled packages are installed before
	 their older non-M-A enabled packages are replaced by newer versions */
      bool const installed = Pkg->CurrentVer != 0;
      if (installed == true && Install(Pkg,FileNames[Pkg->ID]) == false)
	 return false;
      for (PkgIterator P = Pkg.Group().PackageList();
	   P.end() == false; P = Pkg.Group().NextPkg(P))
      {
	 if (P->CurrentVer == 0 || P == Pkg || List->IsFlag(P,pkgOrderList::UnPacked) == true ||
	     Cache[P].InstallVer == 0 || (P.CurrentVer() == Cache[P].InstallVer &&
	      (Cache[Pkg].iFlags & pkgDepCache::ReInstall) != pkgDepCache::ReInstall))
	    continue;
	 if (SmartUnPack(P, false, Depth + 1) == false)
	    return false;
      }
      if (installed == false && Install(Pkg,FileNames[Pkg->ID]) == false)
	 return false;
      for (PkgIterator P = Pkg.Group().PackageList();
	   P.end() == false; P = Pkg.Group().NextPkg(P))
      {
	 if (P->CurrentVer != 0 || P == Pkg || List->IsFlag(P,pkgOrderList::UnPacked) == true ||
	     Cache[P].InstallVer == 0 || (P.CurrentVer() == Cache[P].InstallVer &&
	      (Cache[Pkg].iFlags & pkgDepCache::ReInstall) != pkgDepCache::ReInstall))
	    continue;
	 if (SmartUnPack(P, false, Depth + 1) == false)
	    return false;
      }
   }
   // packages which are already unpacked don't need to be unpacked again
   else if (Pkg.State() != pkgCache::PkgIterator::NeedsConfigure && Install(Pkg,FileNames[Pkg->ID]) == false)
      return false;

   if (Immediate == true) {
      // Perform immedate configuration of the package. 
         if (SmartConfigure(Pkg, Depth + 1) == false)
            _error->Warning(_("Could not perform immediate configuration on '%s'. "
               "Please see man 5 apt.conf under APT::Immediate-Configure for details. (%d)"),Pkg.FullName().c_str(),2);
   }
   
   return true;
}
									/*}}}*/
// PM::OrderInstall - Installation ordering routine			/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgPackageManager::OrderResult pkgPackageManager::OrderInstall()
{
   if (CreateOrderList() == false)
      return Failed;

   Reset();
   
   if (Debug == true)
      clog << "Beginning to order" << endl;

   bool const ordering =
	_config->FindB("PackageManager::UnpackAll",true) ?
		List->OrderUnpack(FileNames) : List->OrderCritical();
   if (ordering == false)
   {
      _error->Error("Internal ordering error");
      return Failed;
   }
   
   if (Debug == true)
      clog << "Done ordering" << endl;

   bool DoneSomething = false;
   for (pkgOrderList::iterator I = List->begin(); I != List->end(); ++I)
   {
      PkgIterator Pkg(Cache,*I);
      
      if (List->IsNow(Pkg) == false)
      {
         if (!List->IsFlag(Pkg,pkgOrderList::Configured) && !NoImmConfigure) {
            if (SmartConfigure(Pkg, 0) == false && Debug)
               _error->Warning("Internal Error, Could not configure %s",Pkg.FullName().c_str());
            // FIXME: The above warning message might need changing
         } else {
	    if (Debug == true)
	       clog << "Skipping already done " << Pkg.FullName() << endl;
	 }
	 continue;
	 
      }
      
      if (List->IsMissing(Pkg) == true)
      {
	 if (Debug == true)
	    clog << "Sequence completed at " << Pkg.FullName() << endl;
	 if (DoneSomething == false)
	 {
	    _error->Error("Internal Error, ordering was unable to handle the media swap");
	    return Failed;
	 }	 
	 return Incomplete;
      }
      
      // Sanity check
      if (Cache[Pkg].Keep() == true && 
	  Pkg.State() == pkgCache::PkgIterator::NeedsNothing &&
	  (Cache[Pkg].iFlags & pkgDepCache::ReInstall) != pkgDepCache::ReInstall)
      {
	 _error->Error("Internal Error, trying to manipulate a kept package (%s)",Pkg.FullName().c_str());
	 return Failed;
      }
      
      // Perform a delete or an install
      if (Cache[Pkg].Delete() == true)
      {
	 if (SmartRemove(Pkg) == false)
	    return Failed;
      }
      else
	 if (SmartUnPack(Pkg,List->IsFlag(Pkg,pkgOrderList::Immediate),0) == false)
	    return Failed;
      DoneSomething = true;
      
      if (ImmConfigureAll) {
         /* ConfigureAll here to pick up and packages left unconfigured becuase they were unpacked in the 
            "PreUnpack Checks" section */
         if (!ConfigureAll())
            return Failed; 
      }
   }

   // Final run through the configure phase
   if (ConfigureAll() == false)
      return Failed;

   // Sanity check
   for (pkgOrderList::iterator I = List->begin(); I != List->end(); ++I)
   {
      if (List->IsFlag(*I,pkgOrderList::Configured) == false)
      {
	 _error->Error("Internal error, packages left unconfigured. %s",
		       PkgIterator(Cache,*I).FullName().c_str());
	 return Failed;
      }
   }
	 
   return Completed;
}
									/*}}}*/
// PM::DoInstallPostFork - Does install part that happens after the fork /*{{{*/
// ---------------------------------------------------------------------
pkgPackageManager::OrderResult 
pkgPackageManager::DoInstallPostFork(int statusFd)
{
      if(statusFd > 0)
         // FIXME: use SetCloseExec here once it taught about throwing
	 //        exceptions instead of doing _exit(100) on failure
	 fcntl(statusFd,F_SETFD,FD_CLOEXEC); 
      bool goResult = Go(statusFd);
      if(goResult == false) 
	 return Failed;

      return Res;
};

// PM::DoInstall - Does the installation				/*{{{*/
// ---------------------------------------------------------------------
/* This uses the filenames in FileNames and the information in the
   DepCache to perform the installation of packages.*/
pkgPackageManager::OrderResult pkgPackageManager::DoInstall(int statusFd)
{
   if(DoInstallPreFork() == Failed)
      return Failed;
   
   return DoInstallPostFork(statusFd);
}
									/*}}}*/	      
