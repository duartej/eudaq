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
            const auto dutname_channel = plane_thhistos.first.getDutnameChannelColRow(0);
            std::string sensorfolder(plane_thhistos.first.getName()+"_"+dutname_channel[0]);
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
        _FillHistograms(simpPlane);
    }
}

TimingHitmapHistos* TimingHitmapCollection::getTimingHitmapHistos(const std::string & sensor, int id) {
    SimpleStandardPlane sp(sensor, id);
    return _map[sp];
}


void TimingHitmapCollection::registerPlane(const SimpleStandardPlane &p) {
    // Create the histogram and associate to the container
    _map[p] = new TimingHitmapHistos(p, _mon);

    if(_mon != nullptr) {
        if(_mon->getOnlineMon() == nullptr) {
            // don't have online mon
            return; 
        }

        const std::string sensor = p.getName();
        const int sensor_id = p.getID();

        // Check the dut name is the same for all the channels
        std::set<std::string> dutname_set;
        std::string dutname;

       for(unsigned int pixid = 0; pixid < p.getBinsX()*p.getBinsY(); ++pixid) {
            auto d_ch_col_row = p.getDutnameChannelColRow(pixid);
            if( d_ch_col_row[0].empty() ) {
                // the pixid is not present, therefore
                // returned empty string
                continue;
            }
            dutname_set.insert( d_ch_col_row[0] );
            dutname = d_ch_col_row[0];
        }
        if( dutname_set.size() > 1) {
            // FIXME  /--> ? EUDAQ_ERROR?
            std::cerr << "Invalid Plane topology, more than one DUT is associated to the" 
                << " same plane" << std::endl;
            // FIXME /-->? return;
        }
        // FIXME -- REMOVE std::cout << "registerPlane:: [" << sensor << "] [" << dutname << "]" << std::endl;
        std::string treename(sensor+"/"+dutname+"/Occupancy");
        _mon->getOnlineMon()->registerTreeItem(treename);
        _mon->getOnlineMon()->registerHisto(treename, 
                getTimingHitmapHistos(sensor,sensor_id)->GetOccupancymapHisto(), 
                "COLZ", 0);
        _mon->getOnlineMon()->addTreeItemSummary(sensor, treename);
        
        treename = sensor+"/"+dutname+"/Channels";
        _mon->getOnlineMon()->registerTreeItem(treename);
        _mon->getOnlineMon()->registerHisto(treename, 
                getTimingHitmapHistos(sensor,sensor_id)->GetChannelmapHisto(), 
                "TEXTCOLZ", 0);
        
        const std::string token(":");
        for(unsigned int pixid = 0 ; pixid < p.getBinsX()*p.getBinsY(); ++pixid) {
            auto d_ch_col_row = p.getDutnameChannelColRow(pixid);
            if( d_ch_col_row[0].empty() ) { 
                // the pixid is not present, therefore
                // returned empty string
                continue;
            }
            const std::string channel = d_ch_col_row[1];
            unsigned int col = std::stoi(d_ch_col_row[2]);
            unsigned int row = std::stoi(d_ch_col_row[3]);
            // FIX<E -- REMOVE std::cout << " ============ col: " << col << " row: " << row << " [" << dutname 
            // FIXME -- REMOVE   << "] [" << channel << "]" << std::endl;

            const std::string fullname = dutname+":"+channel;
            // Amplitude and waveforms, one per channel/pixel
            // FIXME -- OR channel?
            std::string histoname = sensor+"/"+dutname+"/Waveforms/Pixel " + 
                std::to_string(col) + "," + std::to_string(row);
            _mon->getOnlineMon()->registerTreeItem(histoname);
            _mon->getOnlineMon()->registerHisto(histoname,
                    getTimingHitmapHistos(sensor,sensor_id)->getWaveformHisto(pixid), 
                    "COLZ", 0);
            _mon->getOnlineMon()->makeTreeItemSummary(sensor+"/"+dutname+"/Waveforms");
            // And amplitude
            // FIXME -- OR Channel?
            histoname = sensor+"/"+dutname+"/Amplitude/Pixel " + 
                std::to_string(col) + "," + std::to_string(row);
            _mon->getOnlineMon()->registerTreeItem(histoname);
            _mon->getOnlineMon()->registerHisto(histoname,
                    getTimingHitmapHistos(sensor,sensor_id)->GetAmplitudemapHisto(pixid));
            _mon->getOnlineMon()->makeTreeItemSummary(sensor+"/"+dutname+"/Amplitudes");
        }
    }
}
