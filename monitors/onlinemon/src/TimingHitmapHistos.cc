/*
 * TimingTimingHitmapCollection.hh
 *
 *  Created on: June 24, 2013
 *      Author: Jordi Duarte-Campderros (IFCA)
 */
#include "TimingHitmapHistos.hh"
#include "OnlineMon.hh"
#include <cstdlib>

TimingHitmapHistos::TimingHitmapHistos(SimpleStandardPlane p, RootMonitor *mon) 
    : _occupancy_map(nullptr), _channel_map(nullptr)

   {
    
    _wait = false;

    auto dutname_dummychannel = p.getDutNameAndChannel(0);
    _dutname = dutname_dummychannel.first;
    _boardname = p.getName();
    
    _mon = mon;

    // Needed
    if( _boardname.find("CAEN") != std::string::npos ) {
        is_CAENDT5742 = true;
    }

    _maxX = p.getMaxX();
    _maxY = p.getMaxY();

    std::string title(_boardname+" "+_dutname+" Occupancy Map");
    std::string hname("h_"+_boardname+"_"+_dutname+"_occupancymap");
    if(_maxX != -1 && _maxY != -1) {
        _occupancy_map = new TH2I(hname.c_str(), title.c_str(), _maxX, 0, _maxX, _maxY, 0, _maxY);
std::cout << " -->> " << _occupancy_map << std::endl;
        _SetHistoAxisLabels(_occupancy_map, "X", "Y");
        
        title = _boardname+" "+_dutname+" Channel Map";
        hname = "h_"+_boardname+"_"+_dutname+"_channel_map";
        _channel_map = new TH2I(hname.c_str(), title.c_str(), _maxX, 0, _maxX, _maxY, 0, _maxY);

        for(unsigned int col = 0; col < _maxX; ++col) {
            for(unsigned int row = 0; row < _maxY; ++row) {
                const unsigned int pixid = col * _maxY + row;
                auto d_ch = p.getDutNameAndChannel(pixid);
                // Cross-check there is no a filled up pixels
                if( d_ch.second.empty() ) {
                    continue;
                }                     
                title = _boardname+" "+_dutname+" CH:"+d_ch.second+" Waveforms  (Col:"
                    +std::to_string(col)+",Row:"+std::to_string(row);
                hname = "h_waveform_"+_boardname+"_"+_dutname+"_"+std::to_string(pixid);
std::cerr << "BOARD: [" << _boardname << "] [" << _dutname << "]" << " CH:[" << d_ch.second << "]" <<  std::endl;
                // XXX -- HARDCODED!!  I need to extract it from config
                _waveforms[pixid] = new TH2F(hname.c_str(), title.c_str(), 1024, -0.5,1023.5, 200, -0.045, 0.045); 
                _waveforms[pixid]->SetCanExtend(TH1::kAllAxes);

                // amplitude 
                title = _boardname+" "+_dutname+" CH:"+d_ch.second+" Amplitude  (Col:"
                    +std::to_string(col)+",Row:"+std::to_string(row);
                hname = "h_amplitude_"+_boardname+"_"+_dutname+"_"+std::to_string(pixid);
                _amplitude[pixid] = new TH1F(hname.c_str(), title.c_str(), 1000, 0, 1); 
                _amplitude[pixid]->SetCanExtend(TH1::kAllAxes);
                
                std::string chstr = d_ch.second;
                // Be careful, probably contains CH
                if( ! std::isdigit(chstr[0]) ) {
                    chstr.replace(0,2,"");
                }
                // FIXME -- CHeck it again! This is a problematic bug to spot otherwise
                if( ! std::isdigit(chstr[0]) ) {
                    std::cerr << "Unexpected format for the CHANNEL: [" << chstr << "]" <<std::endl;
                }
                int channel = std::stoi(chstr);
    std::cout << "WHAT THE FIUCKE!! " << channel << std::endl;
                // The Channel map, a static map, we can fill it up now
                _channel_map->SetBinContent(col,row,channel);

    std::cout << "WHAT THE FIUCKE!! Afterwards " << channel << std::endl;
            }
        }
    } else {
        std::cerr << "No max sensor size known!" << std::endl;
    }
}


// Use it to dump just a few wf (every 1000)
static int FILLED_WF = 0;

void TimingHitmapHistos::Fill(const SimpleStandardHit &hit) {
    // Be sure it is not a fake pixel (just to fill up the board)
std::cout << "ANTES " << std::endl;
    int pixel_x = hit.getX();
    int pixel_y = hit.getY();
    
    const unsigned int pixid = pixel_x * _maxY + pixel_y;
    if( _waveforms.find(pixid) == _waveforms.end() )
    {
        return;
    }
 
std::cout << "ANTES-1 " << std::endl;
    if(_occupancy_map != nullptr && std::abs(hit.getTOT()) > 0.0) {
        _occupancy_map->Fill(pixel_x, pixel_y);
    }
std::cout << "DESTPUE-21 " << std::endl;


  // FIXME -- Not always, one each 1000 or so?
  // if(FILLED_WF % 500) == 0 ) {
    const std::vector<double> wf = hit.getWaveform();
    const float dt = hit.getWaveformDX();
    std::vector<double> t;
    std::vector<double> _s;
    for(size_t _k = hit.getWaveformX0(); _k < wf.size(); ++_k) {
        t.push_back( _k*dt );
        _s.push_back(_k);
    }
std::cout << "HOLA -098" << std::endl;
std::cout << "HOLA QUIEN? ["<< pixid << "]: " ;
for(const auto & _ll: _waveforms){ std::cout << _ll.first << " ";} std::cout <<std::endl;
    _waveforms[pixid]->FillN(wf.size(), &_s[0], &(hit.getWaveform()[0]), nullptr, 1);
std::cout << "HOLA -23 " << std::endl;
    ++FILLED_WF;
}

void TimingHitmapHistos::Fill(const SimpleStandardPlane &plane) {
    // XXX - Recover this?
    //if(_nHits != nullptr) {
    //    _nHits->Fill(plane.getNHits());
    //}
}


void TimingHitmapHistos::Reset() {
    _occupancy_map->Reset();
    _channel_map->Reset();
    for(auto & id_hwf: _waveforms) {
        id_hwf.second->Reset();
    }
    for(auto & id_hamp: _amplitude) {
        id_hamp.second->Reset();
    }
}

void TimingHitmapHistos::Calculate(const int currentEventNum) {
    // No need to do anything
    _wait = true;
    // ... 
    _wait = false;
}

void TimingHitmapHistos::Write() {
    _occupancy_map->Write();
    _channel_map->Write();
    for(auto & id_hwf: _waveforms) {
        id_hwf.second->Write();
    }
    for(auto & id_hamp: _amplitude) {
        id_hamp.second->Write();
    }
}

int TimingHitmapHistos::_SetHistoAxisLabelx(TH1 *histo, string xlabel) {
    if(histo != nullptr) { 
        histo->GetXaxis()->SetTitle(xlabel.c_str());
    }
    return 0;
}

int TimingHitmapHistos::_SetHistoAxisLabels(TH1 *histo, string xlabel, string ylabel) {
    _SetHistoAxisLabelx(histo, xlabel);
    _SetHistoAxisLabely(histo, ylabel);

    return 0;
}

int TimingHitmapHistos::_SetHistoAxisLabely(TH1 *histo, string ylabel) {
    if(histo != nullptr) {
        histo->GetYaxis()->SetTitle(ylabel.c_str());
    }
    return 0;
}
