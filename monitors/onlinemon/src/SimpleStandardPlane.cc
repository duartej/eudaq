/*
 * SimpleStandardPlane.cxx
 *
 *  Created on: Jun 9, 2011
 *      Author: stanitz
 */

#include <string>
#include <vector>
#include "include/SimpleStandardPlane.hh"

SimpleStandardPlane::SimpleStandardPlane(const std::string &name, const int id,
                                         const int maxX, const int maxY,
                                         OnlineMonConfiguration *mymon)
    : _name(name), _id(id), _maxX(maxX), _maxY(maxY), _binsX(maxX),
      _binsY(maxY), _timingPlane(false) {
  const int hits_reserve = 500;
  _hits.reserve(hits_reserve);
  _badhits.reserve(hits_reserve); // allocate memory
  _clusters.reserve(hits_reserve);
  _rawhits.reserve(hits_reserve);
  for (int i = 0; i < 4; i++) {
    _section_hits[i].reserve(hits_reserve); // FIXME hard-coded for Mimosa
    _section_clusters[i].reserve(hits_reserve);
  }

  mon = mymon;
  AnalogPixelType = false; // per default these are digital pixel planes
  // init these settings
  is_MIMOSA26 = false;
  is_DEPFET = false;
  is_APIX = false;
  is_USBPIX = false;
  is_USBPIXI4 = false;
  is_FORTIS = false;
  is_EXPLORER = false;
  is_APTS = false;
  is_OPAMP = false;
  is_RD53A = false;
  is_RD53B = false;
  is_RD53BQUAD = false;
  is_CAENDT5742 = false;
  is_ETROC = false;
  is_UNKNOWN = true; // per default we don't know this plane
  isRotated = false;
  setPixelType(name); // set the pixel type
}

SimpleStandardPlane::SimpleStandardPlane(const std::string &name, const int id)
    : _name(name), _id(id), _maxX(-1),
      _maxY(-1), _timingPlane(false)
      // FIXME we actually only need this type of constructor to form
                // a map for histogramm allocation
{
  _hits.reserve(400);
  _badhits.reserve(400); //
  _clusters.reserve(40);
  mon = NULL; // no monitor given
  AnalogPixelType = false; // per default these are digital pixel planes
  // init these settings
  is_MIMOSA26 = false;
  is_DEPFET = false;
  is_APIX = false;
  is_USBPIX = false;
  is_USBPIXI4 = false;
  is_FORTIS = false;
  is_EXPLORER = false;
  is_APTS = false;
  is_OPAMP = false;
  is_RD53A = false;
  is_RD53B = false;
  is_RD53BQUAD = false;
  is_CAENDT5742 = false;
  is_ETROC = false;
  is_UNKNOWN = true; // per default we don't know this plane
  isRotated = false;
  setPixelType(name); // set the pixel type
  _binsX = -1;
  _binsY = -1;
}

void SimpleStandardPlane::addHit(SimpleStandardHit oneHit) {
  // oneHit.reduce(_binsX, _binsY); //also fills the appropriate sections
  // //FIXME a better definition of badhits is needed

  _hits.push_back(oneHit);

  if ((oneHit.getX() < 0) || (oneHit.getY() < 0) || (oneHit.getX() > _maxX) ||
      (oneHit.getY() > _maxY)) {
    _badhits.push_back(oneHit);
  }

  if (is_MIMOSA26) {
    int section = oneHit.getX() / mon->getMimosa26_section_boundary();
    if ((section < 0) || (section >= (int)mon->getMimosa26_max_sections())) {
      std::cout << "Error Section invalid: " << section << " " << oneHit.getX()
                << " " << oneHit.getY() << std::endl;
    } else {
      _section_hits[section].push_back(oneHit);
    }
  }
}

void SimpleStandardPlane::addRawHit(SimpleStandardHit oneHit) {
  _rawhits.push_back(oneHit);
}

void SimpleStandardPlane::reducePixels(const int reduceX, const int reduceY) {
  _binsX = reduceX;
  _binsY = reduceY;
}

std::array<std::string,4> SimpleStandardPlane::getDutnameChannelColRow(int index) const {
    // FIXME -- This function is very weak, constantly check the pos value
    
    // XXX - Asumed to be defined with the following information:
    // dutname:channel:colX:rowY
    if( _auxinfo.size() <= index ) {
        return { "", "", "", "" };
    }
    const std::string token(":");
    const std::string fullname = getPixelAuxInfo(index);
    // Nothing is available for this pixel
    if(fullname.empty()) {
        return { "", "", "", "" };
    }

    size_t pos = fullname.find(token);
    const std::string dutname = fullname.substr(0, pos);
    
    // The rest of the string
    const std::string channel_col_row = fullname.substr(pos + token.length());
    // Extract channel (CH<Number>) 
    pos = channel_col_row.find(token);
    const std::string channel = channel_col_row.substr(2,pos-2);
    
    // The rest of the string
    const std::string col_row = channel_col_row.substr(pos + token.length());
    // Extract col (col<Number>)
    pos = col_row.find(token);
    const std::string col = col_row.substr(3,pos-3);
    // And finally row (row<Number>
    const std::string row = col_row.substr(pos + 3 + token.length());

    return { dutname, channel, col, row };
}

void SimpleStandardPlane::doClustering() {
  int nClusters = 0;
  std::vector<int> clusterNumber;
  const int NOCLUSTER = -1000;
  const unsigned int minXDistance = 1;
  const unsigned int minYDistance = 1;
  const unsigned int npixels_hit = _hits.size();
  bool continue_flag;
  clusterNumber.assign(npixels_hit, NOCLUSTER);

  // which planes to cluster, reject planes of Type Fortis
  if (is_FORTIS) {
    return;
  }

  if (npixels_hit > 1) // (npixels_hit < 2000 ))
  {
    std::sort(_hits.begin(), _hits.end(), SortHitsByXY());
    for (unsigned int aPixel = 0; aPixel < npixels_hit; aPixel++) {
      continue_flag = true;
      const SimpleStandardHit &aPix = _hits.at(aPixel);
      for (unsigned int bPixel = aPixel + 1; bPixel < npixels_hit; bPixel++) {
        if (continue_flag != true)
          break;
        const SimpleStandardHit &bPix = _hits.at(bPixel);
        unsigned int xDist = abs(aPix.getX() - bPix.getX());
        unsigned int yDist = abs(aPix.getY() - bPix.getY());

        if ((xDist <= minXDistance) &&
            (yDist <= minYDistance)) { // this means they are neighbors in
                                       // x-direction && / this means they are
                                       // neighbors in y-direction
          if ((clusterNumber.at(aPixel) == NOCLUSTER) &&
              clusterNumber.at(bPixel) == NOCLUSTER) { // none of these pixels
                                                       // have been assigned to
                                                       // a cluster
            //++nClusters;
            clusterNumber.at(aPixel) = ++nClusters;
            clusterNumber.at(bPixel) = nClusters;
          } else if ((clusterNumber.at(aPixel) == NOCLUSTER) &&
                     (clusterNumber.at(bPixel) !=
                      NOCLUSTER)) { // b was assigned already, a not
            clusterNumber.at(aPixel) = clusterNumber.at(bPixel);
          } else if ((clusterNumber.at(aPixel) != NOCLUSTER) &&
                     (clusterNumber.at(bPixel) ==
                      NOCLUSTER)) { // a was assigned already, b not
            clusterNumber.at(bPixel) = clusterNumber.at(aPixel);
          } else { // both pixels have a cluster number already
            int min =
                std::min(clusterNumber.at(aPixel), clusterNumber.at(bPixel));
            clusterNumber.at(aPixel) = min;
            clusterNumber.at(bPixel) = min;
          }
        } else { // these pixels are not neighbored
          continue_flag = false;
        }
      } // inner for loop
    } // outer for loop
    for (unsigned int aPixel = 0; aPixel < npixels_hit; aPixel++)
      if (clusterNumber.at(aPixel) == NOCLUSTER) {
        ++nClusters;
        clusterNumber.at(aPixel) = nClusters;
      }
  } else { // You can't use the clustering algorithm with only one pixel
    if (npixels_hit == 1)
      clusterNumber.at(0) = 1;
  }

  std::set<int> clusterSet(clusterNumber.begin(), clusterNumber.end());

  nClusters = clusterSet.size();

  std::set<int>::iterator it;
  for (it = clusterSet.begin(); it != clusterSet.end(); ++it) {
    SimpleStandardCluster cluster;
    for (unsigned int i = 0; i < npixels_hit; i++) {
      if (clusterNumber.at(i) == *it) { // Put only these pixels in that
                                        // ClusterCollection that belong to that
                                        // cluster
        cluster.addPixel(_hits.at(i));
      }
    }
    _clusters.push_back(cluster);
  }
  // if we have a mimosa, we need to fill the section information

  if (is_MIMOSA26) {
    for (unsigned int mycluster = 0; mycluster < _clusters.size();
         mycluster++) {
      unsigned int cluster_section =
          _clusters[mycluster].getX() / mon->getMimosa26_section_boundary();
      if (cluster_section < mon->getMimosa26_max_sections()) // fixme
      {
        _section_clusters[cluster_section].push_back(_clusters[mycluster]);
      }
    }
  }
}

void SimpleStandardPlane::setPixelType(std::string name) {
  if (name == "MIMOSA26") {
    is_MIMOSA26 = true;
    is_UNKNOWN = false;
  } else if (name == "FORTIS") {
    is_FORTIS = true;
    is_UNKNOWN = false;
  } else if (name == "DEPFET") {
    is_DEPFET = true;
    is_UNKNOWN = false;
    AnalogPixelType = true;
  } else if (name == "APIX") {
    is_APIX = true;
    is_UNKNOWN = false;
    AnalogPixelType = true;
  } else if (name == "USBPIX") {
    is_USBPIX = true;
    is_UNKNOWN = false;
    AnalogPixelType = true;
  } else if (name == "USBPIXI4" || name == "USBPIXI4B") {
    is_USBPIXI4 = true;
    is_UNKNOWN = false;
    AnalogPixelType = true;
  } else if (name.find("USBPIX_GEN") != std::string::npos ) {
    is_USBPIXI4 = true;
    is_UNKNOWN = false;
    AnalogPixelType = true;
  } else if (name == "Explorer20x20" || name == "Explorer30x30") {
    is_EXPLORER = true;
    is_UNKNOWN = false;
    AnalogPixelType = true;
  } else if (name == "pALPIDEfs") {
    is_UNKNOWN = false;
  } else if (name == "ALPIDE") {
    is_UNKNOWN = false;
  } else if (name == "DPTS") {
    is_UNKNOWN = false;
  } else if (name == "CE65") {
    is_UNKNOWN = false;
  } else if (name == "APTS") {
    is_UNKNOWN = false;
    is_APTS = true;
    AnalogPixelType = true;
  } else if (name == "OPAMP") {
    is_UNKNOWN = false;
    is_OPAMP = true;
    AnalogPixelType = true;
  } else if(name.find("Rd53a") != std::string::npos) {
    is_RD53A = true;
    is_UNKNOWN = false;
    AnalogPixelType = true;
  } else if(name.find("Rd53b") != std::string::npos || name.find("RD53B") != std::string::npos) {
    is_RD53B = true;
    is_UNKNOWN = false;
    AnalogPixelType = true;
  } else if(name.find("RD53BQUAD") != std::string::npos) {
    is_RD53BQUAD = true;
    is_UNKNOWN = false;
    AnalogPixelType = true;
  } else if(name.find("CAEN") != std::string::npos) {
    is_CAENDT5742 = true;
    is_UNKNOWN = false;
    AnalogPixelType = true;
  } else if(name.find("ETROC") != std::string::npos ) {
    is_ETROC = true;
    is_UNKNOWN = false;
    AnalogPixelType = true;
  } else {
    is_UNKNOWN = true;
  }
}
