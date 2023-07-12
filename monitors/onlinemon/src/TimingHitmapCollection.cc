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
    // Register the plane if needed
    if(!_IsPlaneRegistered(simpPlane)) {
        registerPlane(simpPlane);
        isOnePlaneRegistered = true;
    }
std::cout << " _FillHistograms: " << simpPlane.getName() << " " << simpPlane.getID()  << std::endl; 
    TimingHitmapHistos* timinghitmap = _map[simpPlane];
    // Not needed so far, maybe will need it at some point?
    // timinghitmap->Fill(simpPlane);
    
    ++counting;
    events += simpPlane.getNHits();

    for(int hitpix = 0; hitpix < simpPlane.getNHits(); hitpix++) {
        const SimpleStandardHit &onehit = simpPlane.getHit(hitpix);
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
    for(int plane = 0; plane < simpev.getNPlanes(); ++plane) {
        const SimpleStandardPlane &simpPlane = simpev.getPlane(plane);
        if(! simpPlane.isTimingPlane()) {
            continue;
        }
std::cout << "FILL [  " << simpPlane.getName() << " " << simpPlane.getID() << " ] " << std::endl;
std::cin.get();
        _FillHistograms(simpPlane);
    }
}

TimingHitmapHistos* TimingHitmapCollection::getTimingHitmapHistos(const std::string & sensor, int id) {
    SimpleStandardPlane sp(sensor, id);  
std::cout << "getTImingHitMaspHistos--> Plane: " << sp.getName() << " " << sp.getID()  << std::endl;
    return _map[sp];
}


void TimingHitmapCollection::registerPlane(const SimpleStandardPlane &p) {
    // Create the histogram and associate to the container
    _map[p] = new TimingHitmapHistos(p, _mon);
std::cout << "registerPlane --> Plane: " << p.getName() << " " << p.getID()  << std::endl;

    if(_mon != nullptr) {
        if(_mon->getOnlineMon() == nullptr) {
            // don't have online mon
            return; 
        }

        const std::string sensor = p.getName();
        
        const std::string token(":");
        for(unsigned int col = 0; col < p.getMaxX(); ++col) {
            for(unsigned int row = 0; row < p.getMaxY(); ++row) {
                const unsigned int pixid = col * p.getMaxY() + row;
                auto d_ch = p.getDutNameAndChannel(pixid);
                const std::string dutname = d_ch.first;
                const std::string channel = d_ch.second;
                if( d_ch.second.empty() ) { 
                    continue;
                }
                const std::string fullname = dutname+":"+channel;
std::cout << "---> " <<  fullname << " in RegisterPlane" << " pixeid:" << pixid << std::endl;
                std::string treename(sensor+"/"+dutname+"/Occupancy");
                _mon->getOnlineMon()->registerTreeItem(treename);
                _mon->getOnlineMon()->registerHisto(treename, 
                        getTimingHitmapHistos(sensor,pixid)->GetOccupancymapHisto(), 
                        "COLZ", 0);
                _mon->getOnlineMon()->addTreeItemSummary(sensor, treename);
                
                treename = sensor+"/"+dutname+"/Channels";
                _mon->getOnlineMon()->registerTreeItem(treename);
                _mon->getOnlineMon()->registerHisto(treename, 
                        getTimingHitmapHistos(sensor,pixid)->GetChannelmapHisto(), 
                        "TEXTCOLZ", 0);

                // Amplitude and waveforms, one per channel/pixel
                // FIXME -- OR channel?
                std::string histoname = sensor+"/"+dutname+"/Waveforms/Pixel " + 
                    std::to_string(col) + "," + std::to_string(row);
                _mon->getOnlineMon()->registerTreeItem(histoname);
                _mon->getOnlineMon()->registerHisto(histoname,
                        getTimingHitmapHistos(sensor,pixid)->getWaveformHisto(pixid), 
                        "COLZ", 0);
                _mon->getOnlineMon()->makeTreeItemSummary(sensor+"/"+dutname+"/Waveforms");
                // And amplitude
                // FIXME -- OR Channel?
                histoname = sensor+"/"+dutname+"/Amplitude/Pixel " + 
                    std::to_string(col) + "," + std::to_string(row);
                _mon->getOnlineMon()->registerTreeItem(histoname);
                _mon->getOnlineMon()->registerHisto(histoname,
                        getTimingHitmapHistos(sensor,pixid)->GetAmplitudemapHisto(pixid));
                _mon->getOnlineMon()->makeTreeItemSummary(sensor+"/"+dutname+"/Amplitudes");
            }
        }
    }
std::cout << "DDONE ===== " << std::endl;
}
