/* This file is part of OpenMalaria.
 * 
 * Copyright (C) 2005-2013 Swiss Tropical and Public Health Institute 
 * Copyright (C) 2005-2013 Liverpool School Of Tropical Medicine
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
#ifndef Hmod_TimedDist
#define Hmod_TimedDist

#include "Global.h"
#include "Population.h"
#include "Host/ImportedInfections.h"
#include "schema/interventions.h"
#include <boost/ptr_container/ptr_vector.hpp>
#include <bitset>

namespace OM {
    using ::boost::ptr_vector;

/** Used to describe which interventions are in use. */
namespace Interventions {
    enum Flags {
        CHANGE_HS,
        CHANGE_EIR,
        VACCINE,        // any vaccine
        MDA,
        IPTI,
        ITN,
        IRS,
        VEC_AVAIL,
        IMMUNE_SUPPRESSION,
        COHORT,
        VECTOR_POP,
        R_0_CASE,
        IMPORTED_INFECTIONS,
        UNINFECT_VECTORS,
        SIZE
    };
}

/** Age-based (continuous) deployment. */
class AgeIntervention {
public:
    AgeIntervention( const ::scnXml::ContinuousDeployment& elt,
        void(Host::Human::*func) (const OM::Population&) );
    
    inline bool operator< (const AgeIntervention& that) const{
        return this->ageTimesteps < that.ageTimesteps;
    }
    
    TimeStep begin, end;    // first timeStep active and first timeStep no-longer active
    TimeStep ageTimesteps;
    bool cohortOnly;
    double coverage;
    // Member function pointer to the function (in Human) responsible for deploying intervention:
    typedef void (Host::Human::*DeploymentFunction) (const OM::Population&);
    DeploymentFunction deploy;
};

/** Interface of a timed intervention. */
class TimedIntervention {
public:
    /// Create, passing time of deployment
    TimedIntervention(TimeStep deploymentTime);
    virtual ~TimedIntervention() {}
    
    inline bool operator< (const TimedIntervention& that) const{
        return this->time < that.time;
    }
    
    virtual void deploy (OM::Population&) =0;
    
    TimeStep time;
};

/** A description of one effect of a human intervention.
 * 
 * Note that one "effect" can have several "actions", but that deployment and
 * decay of these "actions" is usually related.
 * 
 * This is a base class. */
class HumanInterventionEffect {
public:
    /** Deploy the effect to a pre-selected human. */
    virtual void deploy( Host::Human& human ) const =0;
    
protected:
    /** Construct (from a derived class). */
    HumanInterventionEffect() {}
    
private:
    /** Don't copy (this may be possible but shouldn't be needed). */
    HumanInterventionEffect( const HumanInterventionEffect& );
};

/** A description of a human intervention (as a list of effects). */
class HumanIntervention {
public:
    /** Add an effect. */
    inline void addEffect( const HumanInterventionEffect *effect ){ effects.push_back( effect ); }
    
    /** Deploy all effects to a pre-selected human. */
    void deploy( Host::Human& human ) const;
    
private:
    // List of pointers to effects. Does not manage memory (InterventionManager::humanEffects does that).
    vector<const HumanInterventionEffect*> effects;
};

/** Management of interventions deployed on a per-timestep basis. */
class InterventionManager {
public:
    /** Read XML descriptions. */
    InterventionManager (const scnXml::Interventions& intervElt, OM::Population& population);
    
    /// Checkpointing
    template<class S>
    void operator& (S& stream) {
        using namespace OM::util::checkpoint;
        // most members are only set from XML,
        // nextTimed varies but is re-set by loadFromCheckpoint
        importedInfections & stream;
    }

    /** Call after loading a checkpoint, passing the intervention-period time.
     * 
     * Serves to replace health-system and EIR where changeHS/changeEIR
     * interventions have been used. */
    void loadFromCheckpoint( OM::Population& population, OM::util::TimeStep interventionTime );
    
    /// Returns true if any cohort selection "intervention" is active
    inline bool cohortEnabled() const{
        return _cohortEnabled;
    }
    
    /** @brief Deploy interventions
     *
     * Timed interventions are deployed for this timestep.
     * 
     * Continuous interventions are deployed as humans reach the target ages.
     * Unlike with vaccines, missing one schedule doesn't preclude the next. */
    void deploy (OM::Population& population);
    
private:
    // All human intervention effects, indexed by a number. This list is used
    // during initialisation and thereafter only for memory management.
    boost::ptr_vector<HumanInterventionEffect> humanEffects;
    // All human interventions. These are stored here for memory management
    // only (so that they are deleted when this class is destroyed).
    boost::ptr_vector<HumanIntervention> humanInterventions;
    // All continuous interventions, sorted by deployment age (weakly increasing)
    vector<AgeIntervention> ctsIntervs;
    // List of all timed interventions. Should be sorted (time weakly increasing).
    ptr_vector<TimedIntervention> timed;
    uint32_t nextTimed;
    
    // imported infections are not really interventions, and handled by a separate class
    // (but are grouped here for convenience and due toassociation in schema)
    Host::ImportedInfections importedInfections;
    bool _cohortEnabled;
};

}
#endif
