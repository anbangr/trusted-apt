// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
/* ######################################################################

   Simple wrapper around a std::set to provide a similar interface to
   a set of cache structures as to the complete set of all structures
   in the pkgCache. Currently only Package is supported.

   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include <config.h>

#include <apt-pkg/aptconfiguration.h>
#include <apt-pkg/cachefile.h>
#include <apt-pkg/cachefilter.h>
#include <apt-pkg/cacheset.h>
#include <apt-pkg/error.h>
#include <apt-pkg/strutl.h>
#include <apt-pkg/versionmatch.h>
#include <apt-pkg/pkgrecords.h>
#include <apt-pkg/policy.h>

#include <vector>

#include <regex.h>

#include <apti18n.h>
									/*}}}*/
namespace APT {
// FromTask - Return all packages in the cache from a specific task	/*{{{*/
bool PackageContainerInterface::FromTask(PackageContainerInterface * const pci, pkgCacheFile &Cache, std::string pattern, CacheSetHelper &helper) {
	size_t const archfound = pattern.find_last_of(':');
	std::string arch = "native";
	if (archfound != std::string::npos) {
		arch = pattern.substr(archfound+1);
		pattern.erase(archfound);
	}

	if (pattern[pattern.length() -1] != '^')
		return false;
	pattern.erase(pattern.length()-1);

	if (unlikely(Cache.GetPkgCache() == 0 || Cache.GetDepCache() == 0))
		return false;

	bool const wasEmpty = pci->empty();
	if (wasEmpty == true)
		pci->setConstructor(TASK);

	// get the records
	pkgRecords Recs(Cache);

	// build regexp for the task
	regex_t Pattern;
	char S[300];
	snprintf(S, sizeof(S), "^Task:.*[, ]%s([, ]|$)", pattern.c_str());
	if(regcomp(&Pattern,S, REG_EXTENDED | REG_NOSUB | REG_NEWLINE) != 0) {
		_error->Error("Failed to compile task regexp");
		return false;
	}

	bool found = false;
	for (pkgCache::GrpIterator Grp = Cache->GrpBegin(); Grp.end() == false; ++Grp) {
		pkgCache::PkgIterator Pkg = Grp.FindPkg(arch);
		if (Pkg.end() == true)
			continue;
		pkgCache::VerIterator ver = Cache[Pkg].CandidateVerIter(Cache);
		if(ver.end() == true)
			continue;

		pkgRecords::Parser &parser = Recs.Lookup(ver.FileList());
		const char *start, *end;
		parser.GetRec(start,end);
		unsigned int const length = end - start;
		char buf[length];
		strncpy(buf, start, length);
		buf[length-1] = '\0';
		if (regexec(&Pattern, buf, 0, 0, 0) != 0)
			continue;

		pci->insert(Pkg);
		helper.showTaskSelection(Pkg, pattern);
		found = true;
	}
	regfree(&Pattern);

	if (found == false) {
		helper.canNotFindTask(pci, Cache, pattern);
		pci->setConstructor(UNKNOWN);
		return false;
	}

	if (wasEmpty == false && pci->getConstructor() != UNKNOWN)
		pci->setConstructor(UNKNOWN);

	return true;
}
									/*}}}*/
// FromRegEx - Return all packages in the cache matching a pattern	/*{{{*/
bool PackageContainerInterface::FromRegEx(PackageContainerInterface * const pci, pkgCacheFile &Cache, std::string pattern, CacheSetHelper &helper) {
	static const char * const isregex = ".?+*|[^$";
	if (pattern.find_first_of(isregex) == std::string::npos)
		return false;

	bool const wasEmpty = pci->empty();
	if (wasEmpty == true)
		pci->setConstructor(REGEX);

	size_t archfound = pattern.find_last_of(':');
	std::string arch = "native";
	if (archfound != std::string::npos) {
		arch = pattern.substr(archfound+1);
		if (arch.find_first_of(isregex) == std::string::npos)
			pattern.erase(archfound);
		else
			arch = "native";
	}

	if (unlikely(Cache.GetPkgCache() == 0))
		return false;

	APT::CacheFilter::PackageNameMatchesRegEx regexfilter(pattern);

	bool found = false;
	for (pkgCache::GrpIterator Grp = Cache.GetPkgCache()->GrpBegin(); Grp.end() == false; ++Grp) {
		if (regexfilter(Grp) == false)
			continue;
		pkgCache::PkgIterator Pkg = Grp.FindPkg(arch);
		if (Pkg.end() == true) {
			if (archfound == std::string::npos) {
				std::vector<std::string> archs = APT::Configuration::getArchitectures();
				for (std::vector<std::string>::const_iterator a = archs.begin();
				     a != archs.end() && Pkg.end() != true; ++a)
					Pkg = Grp.FindPkg(*a);
			}
			if (Pkg.end() == true)
				continue;
		}

		pci->insert(Pkg);
		helper.showRegExSelection(Pkg, pattern);
		found = true;
	}

	if (found == false) {
		helper.canNotFindRegEx(pci, Cache, pattern);
		pci->setConstructor(UNKNOWN);
		return false;
	}

	if (wasEmpty == false && pci->getConstructor() != UNKNOWN)
		pci->setConstructor(UNKNOWN);

	return true;
}
									/*}}}*/
// FromName - Returns the package defined  by this string		/*{{{*/
pkgCache::PkgIterator PackageContainerInterface::FromName(pkgCacheFile &Cache,
			std::string const &str, CacheSetHelper &helper) {
	std::string pkg = str;
	size_t archfound = pkg.find_last_of(':');
	std::string arch;
	if (archfound != std::string::npos) {
		arch = pkg.substr(archfound+1);
		pkg.erase(archfound);
	}

	if (Cache.GetPkgCache() == 0)
		return pkgCache::PkgIterator(Cache, 0);

	pkgCache::PkgIterator Pkg(Cache, 0);
	if (arch.empty() == true) {
		pkgCache::GrpIterator Grp = Cache.GetPkgCache()->FindGrp(pkg);
		if (Grp.end() == false)
			Pkg = Grp.FindPreferredPkg();
	} else
		Pkg = Cache.GetPkgCache()->FindPkg(pkg, arch);

	if (Pkg.end() == true)
		return helper.canNotFindPkgName(Cache, str);
	return Pkg;
}
									/*}}}*/
// FromString - Return all packages matching a specific string		/*{{{*/
bool PackageContainerInterface::FromString(PackageContainerInterface * const pci, pkgCacheFile &Cache, std::string const &str, CacheSetHelper &helper) {
	bool found = true;
	_error->PushToStack();

	pkgCache::PkgIterator Pkg = FromName(Cache, str, helper);
	if (Pkg.end() == false)
		pci->insert(Pkg);
	else if (FromTask(pci, Cache, str, helper) == false &&
		 FromRegEx(pci, Cache, str, helper) == false)
	{
		helper.canNotFindPackage(pci, Cache, str);
		found = false;
	}

	if (found == true)
		_error->RevertToStack();
	else
		_error->MergeWithStack();
	return found;
}
									/*}}}*/
// FromCommandLine - Return all packages specified on commandline	/*{{{*/
bool PackageContainerInterface::FromCommandLine(PackageContainerInterface * const pci, pkgCacheFile &Cache, const char **cmdline, CacheSetHelper &helper) {
	bool found = false;
	for (const char **I = cmdline; *I != 0; ++I)
		found |= PackageContainerInterface::FromString(pci, Cache, *I, helper);
	return found;
}
									/*}}}*/
// FromModifierCommandLine - helper doing the work for PKG:GroupedFromCommandLine	/*{{{*/
bool PackageContainerInterface::FromModifierCommandLine(unsigned short &modID, PackageContainerInterface * const pci,
							pkgCacheFile &Cache, const char * cmdline,
							std::list<Modifier> const &mods, CacheSetHelper &helper) {
	std::string str = cmdline;
	unsigned short fallback = modID;
	bool modifierPresent = false;
	for (std::list<Modifier>::const_iterator mod = mods.begin();
	     mod != mods.end(); ++mod) {
		size_t const alength = strlen(mod->Alias);
		switch(mod->Pos) {
		case Modifier::POSTFIX:
			if (str.compare(str.length() - alength, alength,
					mod->Alias, 0, alength) != 0)
				continue;
			str.erase(str.length() - alength);
			modID = mod->ID;
			break;
		case Modifier::PREFIX:
			continue;
		case Modifier::NONE:
			continue;
		}
		modifierPresent = true;
		break;
	}
	if (modifierPresent == true) {
		bool const errors = helper.showErrors(false);
		pkgCache::PkgIterator Pkg = FromName(Cache, cmdline, helper);
		helper.showErrors(errors);
		if (Pkg.end() == false) {
			pci->insert(Pkg);
			modID = fallback;
			return true;
		}
	}
	return FromString(pci, Cache, str, helper);
}
									/*}}}*/
// FromModifierCommandLine - helper doing the work for VER:GroupedFromCommandLine	/*{{{*/
bool VersionContainerInterface::FromModifierCommandLine(unsigned short &modID,
							VersionContainerInterface * const vci,
							pkgCacheFile &Cache, const char * cmdline,
							std::list<Modifier> const &mods,
							CacheSetHelper &helper) {
	Version select = NEWEST;
	std::string str = cmdline;
	bool modifierPresent = false;
	unsigned short fallback = modID;
	for (std::list<Modifier>::const_iterator mod = mods.begin();
	     mod != mods.end(); ++mod) {
		if (modID == fallback && mod->ID == fallback)
			select = mod->SelectVersion;
		size_t const alength = strlen(mod->Alias);
		switch(mod->Pos) {
		case Modifier::POSTFIX:
			if (str.compare(str.length() - alength, alength,
					mod->Alias, 0, alength) != 0)
				continue;
			str.erase(str.length() - alength);
			modID = mod->ID;
			select = mod->SelectVersion;
			break;
		case Modifier::PREFIX:
			continue;
		case Modifier::NONE:
			continue;
		}
		modifierPresent = true;
		break;
	}
	if (modifierPresent == true) {
		bool const errors = helper.showErrors(false);
		bool const found = VersionContainerInterface::FromString(vci, Cache, cmdline, select, helper, true);
		helper.showErrors(errors);
		if (found == true) {
			modID = fallback;
			return true;
		}
	}
	return FromString(vci, Cache, str, select, helper);
}
									/*}}}*/
// FromCommandLine - Return all versions specified on commandline	/*{{{*/
bool VersionContainerInterface::FromCommandLine(VersionContainerInterface * const vci,
						pkgCacheFile &Cache, const char **cmdline,
						Version const &fallback, CacheSetHelper &helper) {
	bool found = false;
	for (const char **I = cmdline; *I != 0; ++I)
		found |= VersionContainerInterface::FromString(vci, Cache, *I, fallback, helper);
	return found;
}
									/*}}}*/
// FromString - Returns all versions spedcified by a string		/*{{{*/
bool VersionContainerInterface::FromString(VersionContainerInterface * const vci,
					   pkgCacheFile &Cache, std::string pkg,
					   Version const &fallback, CacheSetHelper &helper,
					   bool const onlyFromName) {
	std::string ver;
	bool verIsRel = false;
	size_t const vertag = pkg.find_last_of("/=");
	if (vertag != std::string::npos) {
		ver = pkg.substr(vertag+1);
		verIsRel = (pkg[vertag] == '/');
		pkg.erase(vertag);
	}
	PackageSet pkgset;
	if (onlyFromName == false)
		PackageContainerInterface::FromString(&pkgset, Cache, pkg, helper);
	else {
		pkgset.insert(PackageContainerInterface::FromName(Cache, pkg, helper));
	}

	bool errors = true;
	if (pkgset.getConstructor() != PackageSet::UNKNOWN)
		errors = helper.showErrors(false);

	bool found = false;
	for (PackageSet::const_iterator P = pkgset.begin();
	     P != pkgset.end(); ++P) {
		if (vertag == std::string::npos) {
			found |= VersionContainerInterface::FromPackage(vci, Cache, P, fallback, helper);
			continue;
		}
		pkgCache::VerIterator V;
		if (ver == "installed")
			V = getInstalledVer(Cache, P, helper);
		else if (ver == "candidate")
			V = getCandidateVer(Cache, P, helper);
		else if (ver == "newest") {
			if (P->VersionList != 0)
				V = P.VersionList();
			else
				V = helper.canNotFindNewestVer(Cache, P);
		} else {
			pkgVersionMatch Match(ver, (verIsRel == true ? pkgVersionMatch::Release :
					pkgVersionMatch::Version));
			V = Match.Find(P);
			if (V.end() == true) {
				if (verIsRel == true)
					_error->Error(_("Release '%s' for '%s' was not found"),
							ver.c_str(), P.FullName(true).c_str());
				else
					_error->Error(_("Version '%s' for '%s' was not found"),
							ver.c_str(), P.FullName(true).c_str());
				continue;
			}
		}
		if (V.end() == true)
			continue;
		helper.showSelectedVersion(P, V, ver, verIsRel);
		vci->insert(V);
		found = true;
	}
	if (pkgset.getConstructor() != PackageSet::UNKNOWN)
		helper.showErrors(errors);
	return found;
}
									/*}}}*/
// FromPackage - versions from package based on fallback		/*{{{*/
bool VersionContainerInterface::FromPackage(VersionContainerInterface * const vci,
					    pkgCacheFile &Cache,
					    pkgCache::PkgIterator const &P,
					    Version const &fallback,
					    CacheSetHelper &helper) {
	pkgCache::VerIterator V;
	bool showErrors;
	bool found = false;
	switch(fallback) {
	case ALL:
		if (P->VersionList != 0)
			for (V = P.VersionList(); V.end() != true; ++V)
				found |= vci->insert(V);
		else
			helper.canNotFindAllVer(vci, Cache, P);
		break;
	case CANDANDINST:
		found |= vci->insert(getInstalledVer(Cache, P, helper));
		found |= vci->insert(getCandidateVer(Cache, P, helper));
		break;
	case CANDIDATE:
		found |= vci->insert(getCandidateVer(Cache, P, helper));
		break;
	case INSTALLED:
		found |= vci->insert(getInstalledVer(Cache, P, helper));
		break;
	case CANDINST:
		showErrors = helper.showErrors(false);
		V = getCandidateVer(Cache, P, helper);
		if (V.end() == true)
			V = getInstalledVer(Cache, P, helper);
		helper.showErrors(showErrors);
		if (V.end() == false)
			found |= vci->insert(V);
		else
			helper.canNotFindInstCandVer(vci, Cache, P);
		break;
	case INSTCAND:
		showErrors = helper.showErrors(false);
		V = getInstalledVer(Cache, P, helper);
		if (V.end() == true)
			V = getCandidateVer(Cache, P, helper);
		helper.showErrors(showErrors);
		if (V.end() == false)
			found |= vci->insert(V);
		else
			helper.canNotFindInstCandVer(vci, Cache, P);
		break;
	case NEWEST:
		if (P->VersionList != 0)
			found |= vci->insert(P.VersionList());
		else
			helper.canNotFindNewestVer(Cache, P);
		break;
	}
	return found;
}
									/*}}}*/
// getCandidateVer - Returns the candidate version of the given package	/*{{{*/
pkgCache::VerIterator VersionContainerInterface::getCandidateVer(pkgCacheFile &Cache,
		pkgCache::PkgIterator const &Pkg, CacheSetHelper &helper) {
	pkgCache::VerIterator Cand;
	if (Cache.IsPolicyBuilt() == true || Cache.IsDepCacheBuilt() == false) {
		if (unlikely(Cache.GetPolicy() == 0))
			return pkgCache::VerIterator(Cache);
		Cand = Cache.GetPolicy()->GetCandidateVer(Pkg);
	} else {
		Cand = Cache[Pkg].CandidateVerIter(Cache);
	}
	if (Cand.end() == true)
		return helper.canNotFindCandidateVer(Cache, Pkg);
	return Cand;
}
									/*}}}*/
// getInstalledVer - Returns the installed version of the given package	/*{{{*/
pkgCache::VerIterator VersionContainerInterface::getInstalledVer(pkgCacheFile &Cache,
		pkgCache::PkgIterator const &Pkg, CacheSetHelper &helper) {
	if (Pkg->CurrentVer == 0)
		return helper.canNotFindInstalledVer(Cache, Pkg);
	return Pkg.CurrentVer();
}
									/*}}}*/

// canNotFindPkgName - handle the case no package has this name		/*{{{*/
pkgCache::PkgIterator CacheSetHelper::canNotFindPkgName(pkgCacheFile &Cache,
			std::string const &str) {
	if (ShowError == true)
		_error->Insert(ErrorType, _("Unable to locate package %s"), str.c_str());
	return pkgCache::PkgIterator(Cache, 0);
}
									/*}}}*/
// canNotFindTask - handle the case no package is found for a task	/*{{{*/
void CacheSetHelper::canNotFindTask(PackageContainerInterface * const pci, pkgCacheFile &Cache, std::string pattern) {
	if (ShowError == true)
		_error->Insert(ErrorType, _("Couldn't find task '%s'"), pattern.c_str());
}
									/*}}}*/
// canNotFindRegEx - handle the case no package is found by a regex	/*{{{*/
void CacheSetHelper::canNotFindRegEx(PackageContainerInterface * const pci, pkgCacheFile &Cache, std::string pattern) {
	if (ShowError == true)
		_error->Insert(ErrorType, _("Couldn't find any package by regex '%s'"), pattern.c_str());
}
									/*}}}*/
// canNotFindPackage - handle the case no package is found from a string/*{{{*/
void CacheSetHelper::canNotFindPackage(PackageContainerInterface * const pci, pkgCacheFile &Cache, std::string const &str) {
}
									/*}}}*/
// canNotFindAllVer							/*{{{*/
void CacheSetHelper::canNotFindAllVer(VersionContainerInterface * const vci, pkgCacheFile &Cache,
		pkgCache::PkgIterator const &Pkg) {
	if (ShowError == true)
		_error->Insert(ErrorType, _("Can't select versions from package '%s' as it is purely virtual"), Pkg.FullName(true).c_str());
}
									/*}}}*/
// canNotFindInstCandVer						/*{{{*/
void CacheSetHelper::canNotFindInstCandVer(VersionContainerInterface * const vci, pkgCacheFile &Cache,
		pkgCache::PkgIterator const &Pkg) {
	if (ShowError == true)
		_error->Insert(ErrorType, _("Can't select installed nor candidate version from package '%s' as it has neither of them"), Pkg.FullName(true).c_str());
}
									/*}}}*/
// canNotFindInstCandVer						/*{{{*/
void CacheSetHelper::canNotFindCandInstVer(VersionContainerInterface * const vci, pkgCacheFile &Cache,
		pkgCache::PkgIterator const &Pkg) {
	if (ShowError == true)
		_error->Insert(ErrorType, _("Can't select installed nor candidate version from package '%s' as it has neither of them"), Pkg.FullName(true).c_str());
}
									/*}}}*/
// canNotFindNewestVer							/*{{{*/
pkgCache::VerIterator CacheSetHelper::canNotFindNewestVer(pkgCacheFile &Cache,
		pkgCache::PkgIterator const &Pkg) {
	if (ShowError == true)
		_error->Insert(ErrorType, _("Can't select newest version from package '%s' as it is purely virtual"), Pkg.FullName(true).c_str());
	return pkgCache::VerIterator(Cache, 0);
}
									/*}}}*/
// canNotFindCandidateVer						/*{{{*/
pkgCache::VerIterator CacheSetHelper::canNotFindCandidateVer(pkgCacheFile &Cache,
		pkgCache::PkgIterator const &Pkg) {
	if (ShowError == true)
		_error->Insert(ErrorType, _("Can't select candidate version from package %s as it has no candidate"), Pkg.FullName(true).c_str());
	return pkgCache::VerIterator(Cache, 0);
}
									/*}}}*/
// canNotFindInstalledVer						/*{{{*/
pkgCache::VerIterator CacheSetHelper::canNotFindInstalledVer(pkgCacheFile &Cache,
		pkgCache::PkgIterator const &Pkg) {
	if (ShowError == true)
		_error->Insert(ErrorType, _("Can't select installed version from package %s as it is not installed"), Pkg.FullName(true).c_str());
	return pkgCache::VerIterator(Cache, 0);
}
									/*}}}*/
// showTaskSelection							/*{{{*/
void CacheSetHelper::showTaskSelection(pkgCache::PkgIterator const &pkg,
				       std::string const &pattern) {
}
									/*}}}*/
// showRegExSelection							/*{{{*/
void CacheSetHelper::showRegExSelection(pkgCache::PkgIterator const &pkg,
					std::string const &pattern) {
}
									/*}}}*/
// showSelectedVersion							/*{{{*/
void CacheSetHelper::showSelectedVersion(pkgCache::PkgIterator const &Pkg,
					 pkgCache::VerIterator const Ver,
					 std::string const &ver,
					 bool const verIsRel) {
}
									/*}}}*/
}
