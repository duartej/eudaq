/*
 * TimingTimingHitmapCollection.hh
 *
 *  Created on: June 24, 2013
 *      Author: Jordi Duarte-Campderros (IFCA)
 */
#include "TimingHitmapCollection.hh"
#include "OnlineMon.hh"

static int counting = 0;
static int events = 0;

bool TimingHitmapCollection::_IsPlaneRegistered(SimpleStandardPlane p) {
    std::map<SimpleStandardPlane, TimingHitmapHistos *>::iterator it;
    it = _map.find(p);
    return (it != _map.end());
}

void TimingHitmapCollection::_FillHistograms(const SimpleStandardPlane &simpPlane) {
std::cout << " --> Before re-gisterd " << std::endl;
    // Register the plane if needed
    if(!_IsPlaneRegistered(simpPlane)) {
std::cout << " --> Inside " << std::endl;
        registerPlane(simpPlane);
        isOnePlaneRegistered = true;
    }
    
std::cout << " --> After re-gisterd , define the histos" << std::endl;
    TimingHitmapHistos* timinghitmap = _map[simpPlane];
    // Not needed so far, maybe will need it at some point?
    // timinghitmap->Fill(simpPlane);
    
    ++counting;
    events += simpPlane.getNHits();
std::cout << " --> Before Hits " << events << std::endl;

std::cout << " --> Entrando en el loop " << events << std::endl;
    for(int hitpix = 0; hitpix < simpPlane.getNHits(); hitpix++) {
        const SimpleStandardHit &onehit = simpPlane.getHit(hitpix);
std::cout << " --> Filling the hit " << events << std::endl;
        timinghitmap->Fill(onehit);
    }
}

void TimingHitmapCollection::bookHistograms(const SimpleStandardEvent &simpev) {
    for(int plane = 0; plane < simpev.getNPlanes(); plane++) {
        SimpleStandardPlane simpPlane = simpev.getPlane(plane);
        if(!_IsPlaneRegistered(simpPlane)) {
            registerPlane(simpPlane);
        }
    }
}

void TimingHitmapCollection::Write(TFile *file) {
    if (file == nullptr) {
        std::cout << "TimingHitmapCollection::Write File pointer is `nullptr`"<< std::endl;
        exit(-1);
    }
    
    
    if(gDirectory)
    {
        gDirectory->mkdir("TimingHitmaps");
        gDirectory->cd("TimingHitmaps");

        for(const auto & plane_thhistos: _map) {
            // FIXME -- Correct?
            const auto dutname_channel = plane_thhistos.first.getDutNameAndChannel(0);
            std::string sensorfolder(plane_thhistos.first.getName()+"_"+dutname_channel.first);
            gDirectory->mkdir(sensorfolder.c_str());
            gDirectory->cd(sensorfolder.c_str());
            plane_thhistos.second->Write();
            gDirectory->cd("..");
        }
        gDirectory->cd("..");
    }
}

void TimingHitmapCollection::Calculate(const unsigned int currentEventNumber) {
    if( (currentEventNumber > 10 && currentEventNumber % 1000 * _reduce == 0) ) {
        std::map<SimpleStandardPlane, TimingHitmapHistos *>::iterator it;
        for (it = _map.begin(); it != _map.end(); ++it) {
            it->second->Calculate(currentEventNumber / _reduce);
        }
    }
}

void TimingHitmapCollection::Reset() {
    std::map<SimpleStandardPlane, TimingHitmapHistos *>::iterator it;
    for(auto & it: _map) {
        it.second->Reset();
    }
}

void TimingHitmapCollection::Fill(const SimpleStandardEvent &simpev) {
std::cout << "Fill: Antes del loop" << std::endl;
    for(int plane = 0; plane < simpev.getNPlanes(); ++plane) {
        const SimpleStandardPlane &simpPlane = simpev.getPlane(plane);
        if(! simpPlane.isTimingPlane()) {
            continue;
        }

std::cout << "Fill: Antes del _FillHistograms at plane-" << plane << " " << simpPlane.getName() << " " << simpPlane.getID() <<   std::endl;
        _FillHistograms(simpPlane);
    }
}

TimingHitmapHistos* TimingHitmapCollection::getTimingHitmapHistos(const std::string & sensor) {
    SimpleStandardPlane sp(sensor, 0);  
    return _map[sp];
}


void TimingHitmapCollection::registerPlane(const SimpleStandardPlane &p) {
std::cout << " INSIDE REGISTER" << std::endl;
    // Create the histogram and associate to the container
    _map[p] = new TimingHitmapHistos(p, _mon);
std::cout << " INSIDE REGISTER -1" << std::endl;

    if(_mon != nullptr) {
        if(_mon->getOnlineMon() == nullptr) {
            // don't have online mon
            return; 
        }
std::cout << " INSIDE REGISTER -2" << std::endl;
        
        // Extract the name of the DUT, it is passed in the plane name
        // as DUTNAME:
        std::map<std::string, bool> activate;
        const std::string token(":");
        for(unsigned int col = 0; col < p.getMaxX(); ++col) {
            for(unsigned int row = 0; row < p.getMaxY(); ++row) {
std::cout << " INSIDE REGISTER -3 " << col << " " << row << std::endl;
                const unsigned int pixid = col * p.getMaxY() + row;
                /// FIXME: Use the GetDutAnd.. funciton
                auto d_ch = p.getDutNameAndChannel(pixid);
                const std::string dutname = d_ch.first;
                const std::string channel = d_ch.second;
                if( d_ch.second.empty() ) { 
                    continue;
                }
                const std::string fullname = dutname+":"+channel;

std::cout << " INSIDE REGISTER : " << fullname << std::endl;
                // Just one per sensor
                if(! activate[fullname]) {
std::cout << " INSIDE REGISTER A: " << fullname << std::endl;
                    std::string treename(p.getName()+"/"+dutname+"/Occupancy");
std::cout << " INSIDE REGISTER B: " << fullname << std::endl;
                    _mon->getOnlineMon()->registerTreeItem(treename);
std::cout << " INSIDE REGISTER C: " << fullname << std::endl;
                    _mon->getOnlineMon()->registerHisto(treename, 
                            getTimingHitmapHistos(dutname)->GetOccupancymapHisto(), 
                            "COLZ", 0);
std::cout << " INSIDE REGISTER D: " << fullname << std::endl;
                    _mon->getOnlineMon()->addTreeItemSummary(p.getName(), treename);

std::cout << " INSIDE REGISTER F: " << fullname << std::endl;
                    treename = p.getName()+"/"+dutname+"/Channels";
std::cout << " INSIDE REGISTER G: " << fullname << std::endl;
                    _mon->getOnlineMon()->registerTreeItem(treename);
std::cout << " INSIDE REGISTER H: " << fullname << std::endl;
                    _mon->getOnlineMon()->registerHisto(treename, 
                            getTimingHitmapHistos(dutname)->GetChannelmapHisto(), 
                            "TEXTCOLZ", 0);
                    activate[fullname] = true;
                }

std::cout << " INSIDE REGISTER 2: " << fullname << std::endl;

                // Amplitude and waveforms, one per channel/pixel
                // FIXME -- OR channel?
                std::string histoname = p.getName()+"/"+dutname+"/Waveforms/Pixel " + 
                    std::to_string(col) + "," + std::to_string(row);
std::cout << " INSIDE REGISTER 3: " << fullname << std::endl;
                _mon->getOnlineMon()->registerTreeItem(histoname);
std::cout << " INSIDE REGISTER 4: " << fullname << std::endl;
                _mon->getOnlineMon()->registerHisto(histoname,
                        getTimingHitmapHistos(dutname)->getWaveformHisto(pixid), 
                        "COLZ", 0);
std::cout << " INSIDE REGISTER 5: " << fullname << std::endl;
                _mon->getOnlineMon()->makeTreeItemSummary(p.getName()+"/"+dutname+"/Waveforms");
std::cout << " INSIDE REGISTER 3: " << fullname << std::endl;
                // And amplitude
                // FIXME -- OR Channel?
                histoname = p.getName()+"/"+dutname+"/Amplitude/Pixel " + 
                    std::to_string(col) + "," + std::to_string(row);
                _mon->getOnlineMon()->registerTreeItem(histoname);
                _mon->getOnlineMon()->registerHisto(histoname,
                        getTimingHitmapHistos(dutname)->GetAmplitudemapHisto(pixid));
                _mon->getOnlineMon()->makeTreeItemSummary(p.getName()+"/"+dutname+"/Amplitudes");
            }
        }
    }
}
