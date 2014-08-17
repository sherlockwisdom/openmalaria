/* This file is part of OpenMalaria.
 * 
 * Copyright (C) 2005-2014 Swiss Tropical and Public Health Institute
 * Copyright (C) 2005-2014 Liverpool School Of Tropical Medicine
 * 
 * OpenMalaria is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "Global.h"
#include "WithinHost/DescriptiveWithinHost.h"
#include "WithinHost/Diagnostic.h"
#include "util/ModelOptions.h"
#include "PopulationStats.h"
#include "util/StreamValidator.h"
#include "util/errors.h"
#include <cassert>

using namespace std;

namespace OM {
namespace WithinHost {

extern bool bugfix_max_dens;    // DescriptiveInfection.cpp

// -----  Initialization  -----

DescriptiveWithinHostModel::DescriptiveWithinHostModel( double comorbidityFactor ) :
        WHFalciparum( comorbidityFactor )
{
    assert( TimeStep::interval == 5 );
}

DescriptiveWithinHostModel::~DescriptiveWithinHostModel() {
    for( list<DescriptiveInfection*>::iterator inf = infections.begin(); inf != infections.end(); ++inf ){
        delete *inf;
    }
    infections.clear();
}


// -----  Simple infection adders/removers  -----

DescriptiveInfection* DescriptiveWithinHostModel::createInfection () {
    return new DescriptiveInfection();
}
void DescriptiveWithinHostModel::loadInfection(istream& stream) {
    infections.push_back(new DescriptiveInfection(stream));
}

void DescriptiveWithinHostModel::clearInfections( Treatments::Stages stage ){
    for (std::list<DescriptiveInfection*>::iterator inf = infections.begin(); inf != infections.end();) {
        if( stage == Treatments::BOTH ||
            (stage == Treatments::LIVER && !(*inf)->bloodStage()) ||
            (stage == Treatments::BLOOD && (*inf)->bloodStage())
        ){
            delete *inf;
            inf = infections.erase( inf );
        }else{
            ++inf;
        }
    }
    numInfs = infections.size();
}

// -----  Interventions  -----

void DescriptiveWithinHostModel::clearImmunity() {
    for (std::list<DescriptiveInfection*>::iterator inf = infections.begin(); inf != infections.end(); ++inf) {
        (*inf)->clearImmunity();
    }
    _cumulativeh = 0.0;
    _cumulativeYlag = 0.0;
}
void DescriptiveWithinHostModel::importInfection(){
    PopulationStats::totalInfections += 1;
    if( numInfs < MAX_INFECTIONS ){
        PopulationStats::allowedInfections += 1;
        _cumulativeh += 1;
        numInfs += 1;
        infections.push_back(createInfection());
    }
    assert( numInfs == static_cast<int>(infections.size()) );
}


// -----  Density calculations  -----

void DescriptiveWithinHostModel::update(int nNewInfs, double ageInYears, double bsvFactor) {
    // Cache total density for infectiousness calculations
    _ylag[mod_nn(TimeStep::simulation.asInt(),_ylagLen)] = totalDensity;
    
    // Note: adding infections at the beginning of the update instead of the end
    // shouldn't be significant since before latentp delay nothing is updated.
    PopulationStats::totalInfections += nNewInfs;
    nNewInfs=min(nNewInfs,MAX_INFECTIONS-numInfs);
    PopulationStats::allowedInfections += nNewInfs;
    numInfs += nNewInfs;
    assert( numInfs>=0 && numInfs<=MAX_INFECTIONS );
    for ( int i=0; i<nNewInfs; ++i ) {
        infections.push_back(createInfection());
    }
    assert( numInfs == static_cast<int>(infections.size()) );

    updateImmuneStatus ();

    totalDensity = 0.0;
    timeStepMaxDensity = 0.0;

    // As in AJTMH p22, cumulativeh (X_h + 1) doesn't include infections added
    // this time-step and cumulativeY only includes past densities.
    double cumulativeh=_cumulativeh;
    double cumulativeY=_cumulativeY;
    _cumulativeh += nNewInfs;
    
    bool treatmentLiver = treatExpiryLiver >= TimeStep::simulation;
    bool treatmentBlood = treatExpiryBlood >= TimeStep::simulation;
    
    for (std::list<DescriptiveInfection*>::iterator inf = infections.begin(); inf != infections.end();) {
        //NOTE: it would be nice to combine this code with that in
        // CommonWithinHost.cpp, but a few changes would be needed:
        // INNATE_MAX_DENS and MAX_DENS_CORRECTION would need to be required
        // (couldn't support old parameterisations using buggy versions of code
        // any more).
        // SP drug action and the PK/PD model would need to be abstracted
        // behind a common interface.
        if ( (*inf)->expired() /* infection has self-terminated */ ||
            ((*inf)->bloodStage() ? treatmentBlood : treatmentLiver) )
        {
            delete *inf;
            inf=infections.erase(inf);
            numInfs--;
            continue;
        }
        
        // Should be: infStepMaxDens = 0.0, but has some history.
        // See MAX_DENS_CORRECTION in DescriptiveInfection.cpp.
        double infStepMaxDens = timeStepMaxDensity;
        (*inf)->determineDensities(ageInYears, cumulativeh, cumulativeY, infStepMaxDens, _innateImmSurvFact, bsvFactor);

        if (bugfix_max_dens)
            infStepMaxDens = std::max(infStepMaxDens, timeStepMaxDensity);
        timeStepMaxDensity = infStepMaxDens;

        double density = (*inf)->getDensity();
        totalDensity += density;
        _cumulativeY += TimeStep::interval * density;

        ++inf;
    }
    util::streamValidate( totalDensity );
    assert( (boost::math::isfinite)(totalDensity) );        // inf probably wouldn't be a problem but NaN would be
}

void DescriptiveWithinHostModel::addProphylacticEffects(const vector<double>& pClearanceByTime) {
    throw util::xml_scenario_error( "Please enable PROPHYLACTIC_DRUG_ACTION_MODEL" );
}


// -----  Summarize  -----

WHInterface::InfectionCount DescriptiveWithinHostModel::countInfections () const{
    InfectionCount count;       // constructor initialises counts to 0
    count.total = infections.size();
    for (std::list<DescriptiveInfection*>::const_iterator inf = infections.begin(); inf != infections.end(); ++inf) {
        if (Diagnostic::default_.isPositive( (*inf)->getDensity() ) )
            count.patent += 1;
    }
    return count;
}


// -----  Data checkpointing  -----

void DescriptiveWithinHostModel::checkpoint (istream& stream) {
    WHFalciparum::checkpoint (stream);
    for (int i=0; i<numInfs; ++i) {
        loadInfection(stream);  // create infections using a virtual function call
    }
    assert( numInfs == static_cast<int>(infections.size()) );
}
void DescriptiveWithinHostModel::checkpoint (ostream& stream) {
    WHFalciparum::checkpoint (stream);
    BOOST_FOREACH (DescriptiveInfection* inf, infections) {
        (*inf) & stream;
    }
}

char const*const not_impl = "feature not available with the \"descriptive\" within-host model";
void DescriptiveWithinHostModel::treatPkPd(size_t schedule, size_t dosages){
    throw TRACED_EXCEPTION( not_impl, util::Error::WHFeatures ); }

}
}
