/*
 * TimingHitmapCollection.hh
 *
 *  Created on: June 24, 2013
 *      Author: Jordi Duarte-Campderros (IFCA)
 */

#ifndef TIMINGHITMAPHISTOS_HH_
#define TIMINGHITMAPHISTOS_HH_

#include <TH2I.h>
#include <TH2F.h>
#include <TH2D.h>
#include <TH1F.h>
#include <TFile.h>

#include "SimpleStandardEvent.hh"

class RootMonitor;

class TimingHitmapHistos {
    protected:
        // Sensor topography
        std::string _dutname;
        std::string _boardname;
        // DEPRECATED?? FIXME
        int _id;
        int _maxX;
        int _maxY;
        

        bool _wait;
        TH2I *_occupancy_map;
        TH2I *_channel_map;
        std::map<int, TH1F*> _amplitude;
        //std::map<int, TH1F*> _baseline;
        // TH1F* _toa;
        // ???
        std::map<int,TH2F*> _waveforms;
    
    public:
        TimingHitmapHistos(SimpleStandardPlane p, RootMonitor *mon);
        void Fill(const SimpleStandardHit &hit);
        void Fill(const SimpleStandardPlane &plane);
        void Reset();
        
        void Calculate(const int currentEventNum);
        void Write();

        TH2I* GetOccupancymapHisto() { if( _occupancy_map == nullptr ) { std::cout << " PP" << std::endl; std::cin.get() ; } std::cout << "LL" << std::endl; return _occupancy_map; }
        TH2I* GetChannelmapHisto() { return _channel_map; }
        TH1F* GetAmplitudemapHisto(unsigned int pixel_id) { return _amplitude[pixel_id]; }
        // Per pixel
        TH2F * getWaveformHisto(unsigned int pixel_id) { return _waveforms[pixel_id]; }
        
        void setRootMonitor(RootMonitor *mon) { _mon = mon; }

    private:
        // Helper histo functions
        int _SetHistoAxisLabelx(TH1 *histo, std::string xlabel);
        int _SetHistoAxisLabely(TH1 *histo, std::string ylabel);
        int _SetHistoAxisLabels(TH1 *histo, std::string xlabel, std::string ylabel);
        // Store an array representing the map
        int **plane_map_array; 
        // We don't need occupancy to be refreshed for every
        // single event
        int filling_counter; 
        
        RootMonitor *_mon;
        // check what kind sensor we're dealing with
        // for the filling this eliminates a string comparison
        bool is_CAENDT5742;
};

#ifdef __CINT__
#pragma link C++ class TimingHitmapHistos - ;
#endif

#endif /* TIMINGHITMAPHISTOS_HH_ */
