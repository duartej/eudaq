/*
 * TimingHitmapCollection.hh
 *
 *  Created on: June 24, 2013
 *      Author: Jordi Duarte-Campderros (IFCA)
 */

#ifndef TIMINGHITMAPCOLLECTION_HH_
#define TIMINGHITMAPCOLLECTION_HH_
// Project Includes
#include "SimpleStandardEvent.hh"
#include "TimingHitmapHistos.hh"
#include "BaseCollection.hh"

// ROOT Includes
#include <RQ_OBJECT.h>
#include <TFile.h>

// STL includes
#include <string>
#include <map>


class TimingHitmapCollection : public BaseCollection { 
    RQ_OBJECT("TimingHitmapCollection")
    
    protected:
        std::map<SimpleStandardPlane, TimingHitmapHistos*> _map;
        
        bool isOnePlaneRegistered;
        bool _IsPlaneRegistered(SimpleStandardPlane p);
        void _FillHistograms(const SimpleStandardPlane &simpPlane);
        
    public:
        TimingHitmapCollection() : BaseCollection() {
            isOnePlaneRegistered = false;
            CollectionType = TIMINGHITMAP_COLLECTION_TYPE;
        }
        void setRootMonitor(RootMonitor *mon) { _mon = mon; }
        void registerPlane(const SimpleStandardPlane &p);
        void bookHistograms(const SimpleStandardEvent &simpev);
        TimingHitmapHistos* getTimingHitmapHistos(const std::string & dut);
        void Fill(const SimpleStandardEvent &simpev) override;
        void Reset() override;
        virtual void Write(TFile *file) override;
        virtual void Calculate(const unsigned int currentEventNumber) override;
};

#ifdef __CINT__
#pragma link C++ class TimingHitmapCollection - ;
#endif

#endif /* TIMINGHITMAPCOLLECTION_HH_ */
