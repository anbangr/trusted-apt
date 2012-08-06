#include<config.h>

#include <iostream>
#include <apt-pkg/error.h>
#include <apt-pkg/vendor.h>
#include <apt-pkg/configuration.h>

Vendor::Vendor(std::string VendorID,
               std::string Origin,
	       std::vector<struct Vendor::Fingerprint *> *FingerprintList)
{
   this->VendorID = VendorID;
   this->Origin = Origin;
   for (std::vector<struct Vendor::Fingerprint *>::iterator I = FingerprintList->begin();
	I != FingerprintList->end(); I++)
   {
      if (_config->FindB("Debug::Vendor", false))
         std::cerr << "Vendor \"" << VendorID << "\": Mapping \""
		   << (*I)->Print << "\" to \"" << (*I)->Description << '"' << std::endl;
      Fingerprints[(*I)->Print] = (*I)->Description;
   }
   delete FingerprintList;
}

const std::string Vendor::LookupFingerprint(std::string Print) const
{
   std::map<std::string,std::string>::const_iterator Elt = Fingerprints.find(Print);
   if (Elt == Fingerprints.end())
      return "";
   else
      return (*Elt).second;
}

bool Vendor::CheckDist(std::string Dist)
{
   return true;
}
