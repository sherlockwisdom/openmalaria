/* This file is part of OpenMalaria.
 * 
 * Copyright (C) 2005-2015 Swiss Tropical and Public Health Institute
 * Copyright (C) 2005-2015 Liverpool School Of Tropical Medicine
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

#ifndef Hmod_SimpleMPDAnophelesModel
#define Hmod_SimpleMPDAnophelesModel

#include "Transmission/Anopheles/AnophelesModel.h"

namespace OM {
namespace Transmission {
namespace Anopheles {

class SimpleMPDAnophelesModel : public AnophelesModel
{
public:
    ///@brief Initialisation and destruction
    //@{
    SimpleMPDAnophelesModel (const scnXml::SimpleMPD& elt)
    {
        quinquennialOvipositing.assign (SimTime::fromYearsI(5), 0.0);
        invLarvalResources.assign (SimTime::oneYear(), 0.0);
        
        developmentDuration = SimTime::fromDays(elt.getDevelopmentDuration().getValue());
        if (!(developmentDuration > SimTime::zero()))
            throw util::xml_scenario_error("entomology.vector.simpleMPD.developmentDuration: "
                "must be positive");
        probPreadultSurvival = elt.getDevelopmentSurvival().getValue();
        if (!(0.0 <= probPreadultSurvival && probPreadultSurvival <= 1.0))
            throw util::xml_scenario_error("entomology.vector.simpleMPD.developmentSurvival: "
                "must be a probability (in range [0,1]");
        fEggsLaidByOviposit = elt.getFemaleEggsLaidByOviposit().getValue();
        if (!(fEggsLaidByOviposit > 0.0))
            throw util::xml_scenario_error("entomology.vector.simpleMPD.femaleEggsLaidByOviposit: "
                "must be positive");
        nOvipositingDelayed.assign (developmentDuration, 0.0);
    }

    /** Initialisation which must wait until a human population is available.
     * This is only called when a checkpoint is not loaded.
     *
     * @param nHumans Human population size
     * @param meanPopAvail The mean availability of age-based relative
     * availability of humans to mosquitoes across populations.
     * @param sum_avail sum_i α_i * N_i for human hosts i
     * @param sigma_f sum_i α_i * N_i * P_Bi for human hosts i
     * @param sigma_df sum_i α_i * N_i * P_Bi * P_Ci * P_Di for human hosts i
     * @param sigma_dff sum_i α_i * N_i * P_Bi * P_Ci * P_Di * rel_mosq_fecundity for human hosts i
     *
     * Can only usefully run its calculations when not checkpointing, due to
     * population not being the same when loaded from a checkpoint. */
    virtual void init2 (int nHumans, double meanPopAvail, double sum_avail, double sigma_f, double sigma_df, double sigma_dff)
    {
        AnophelesModel::init2(nHumans, meanPopAvail, sum_avail, sigma_f, sigma_df, sigma_dff);

        // Recompute tsp_dff locally
        double leaveRate = sum_avail + nhh_avail + mosqSeekingDeathRate;
        sigma_df += nhh_sigma_df;
        sigma_dff += nhh_sigma_dff;
        
        double tsP_A = exp(-leaveRate * mosqSeekingDuration);
        double availDivisor = (1.0 - tsP_A) / leaveRate;   // α_d
        double tsP_dff  = sigma_dff * availDivisor * probMosqSurvivalOvipositing;

        // Initialise nOvipositingDelayed
        SimTime y1 = SimTime::oneYear();
        SimTime tau = mosqRestDuration;
        for( SimTime t = SimTime::zero(); t < developmentDuration; t += SimTime::oneDay() ){
            nOvipositingDelayed[mod_nn(t+tau, developmentDuration)] =
                tsP_dff * initNvFromSv * forcedS_v[t];
        }

        // Used when calculating invLarvalResources (but not a hard constraint):
        assert(tau+developmentDuration <= y1);
        for( SimTime t = SimTime::zero(); t < SimTime::oneYear(); t += SimTime::oneDay() ){
            double yt = fEggsLaidByOviposit * tsP_dff * initNvFromSv *
                forcedS_v[mod_nn(t + y1 - tau - developmentDuration, y1)];
            invLarvalResources[t] = (probPreadultSurvival * yt - mosqEmergeRate[t]) /
                (mosqEmergeRate[t] * yt);
        }
    }
    
    virtual void scale(double factor)
    {
        AnophelesModel::scale(factor);
        vectors::scale (nOvipositingDelayed, factor);
    }

    /** Work out whether another interation is needed for initialisation and if
     * so, make necessary changes.
     *
     * @returns true if another iteration is needed. */
    virtual bool initIterate ()
    {
        bool fitted = AnophelesModel::initIterate();

        SimTime y1 = SimTime::oneYear(),
            y2 = SimTime::fromYearsI(2),
            y3 = SimTime::fromYearsI(3),
            y4 = SimTime::fromYearsI(4),
            y5 = SimTime::fromYearsI(5);
        assert(mosqEmergeRate.size() == y1);
        
        for( SimTime t = SimTime::zero(); t < y1; t += SimTime::oneDay() ){
            SimTime ttj = t - developmentDuration;
            // b · P_df · avg_N_v(t - θj - τ):
            double yt = fEggsLaidByOviposit * 0.2 * (
                quinquennialOvipositing[ttj + y1] +
                quinquennialOvipositing[ttj + y2] +
                quinquennialOvipositing[ttj + y3] +
                quinquennialOvipositing[ttj + y4] +
                quinquennialOvipositing[mod_nn(ttj + y5, y5)]);
            invLarvalResources[t] = (probPreadultSurvival * yt - mosqEmergeRate[t]) /
                (mosqEmergeRate[t] * yt);
        }

        return fitted;
    }
    //@}
    
    virtual double getEmergenceRate(const SimTime &d0, const vecDay<double>& mosqEmergeRate, double nOvipositing)
    {
        // Simple Mosquito Population Dynamics model: emergence depends on the
        // adult population, resources available, and larviciding.
        // See: A Simple Periodically-Forced Difference Equation Model for
        // Mosquito Population Dynamics, N. Chitnis, 2012. TODO: publish & link.
        SimTime d1 = d0 + SimTime::oneDay();

        double yt = fEggsLaidByOviposit * nOvipositingDelayed[mod_nn(d1, developmentDuration)];
        double emergence = probPreadultSurvival * yt / (1.0 + invLarvalResources[mod_nn(d0, SimTime::oneYear())] * yt);
        nOvipositingDelayed[mod_nn(d1, developmentDuration)] = nOvipositing;
        quinquennialOvipositing[mod_nn(d1, SimTime::fromYearsI(5))] = nOvipositing;
        return emergence;
    }

    ///@brief Interventions and reporting
    //@{
    virtual double getResAvailability() const
    {
        //TODO: why offset by one time step? This is effectively getting the resources available on the last time step
        //TODO: only have to add one year because of offset
        SimTime start = sim::now() - SimTime::oneTS() + SimTime::oneYear();
        double total = 0;
        for( SimTime i = start, end = start + SimTime::oneTS(); i < end; i += SimTime::oneDay() ){
            SimTime dYear1 = mod_nn(i, SimTime::oneYear());
            total += 1.0 / invLarvalResources[dYear1];
        }
        return total / SimTime::oneTS().inDays();
    }

    virtual double getResRequirements() const {
        return numeric_limits<double>::quiet_NaN();
    }

    virtual void checkpoint (istream& stream){ (*this) & stream; }
    virtual void checkpoint (ostream& stream){ (*this) & stream; }

private:
    template<class S>
    void operator& (S& stream) {
        AnophelesModel::checkpoint(stream);

        quinquennialOvipositing & stream;
        developmentDuration & stream;
        probPreadultSurvival & stream;
        fEggsLaidByOviposit & stream;
        invLarvalResources & stream;
        nOvipositingDelayed & stream;
    }

    // -----  model parameters (loaded from XML)  -----
    
    /** Duration of development (time from egg laying to emergence) in days. */
    SimTime developmentDuration;
    
    /** Survival probability of a mosquito from egg to emergence in the absence
     * of density dependent mortality. */
    double probPreadultSurvival;
    
    /** Mean number of female eggs laid when a mosquito oviposites. */
    double fEggsLaidByOviposit;
    
    
    // -----  parameters (constant after initialisation)  -----
    
    /** As quinquennialS_v, but for N_v*P_df (units: animals). */
    vecDay<double> quinquennialOvipositing;
    
    
    /** Resources for mosquito larvae (or rather 1 over resources); γ(t) in
     * model description.
     * 
     * Unlike model description, we allow special values 0 for no density
     * dependence and infinity for zero emergence.
     * 
     * Index t should correspond to the resources available to mosquitoes
     * emerging at t (i.e. same index in mosqEmergeRate).
     * 
     * Has annual periodicity: length is 365. First value (index 0) corresponds
     * to first day of year (1st Jan or something else if rebased). In 5-day
     * time-step model values at indecies 0 through 4 are used to calculate the
     * state at time-step 1.
     * 
     * Units: 1 / animals per day.
     *
     * vecDay be checkpointed. */
    vecDay<double> invLarvalResources;
    
    /** Vector for storing values of nOvipositing for the last
     * developmentDuration time steps. Index 0 should correspond to
     * nOvipositing developmentDuration days before
     * get(0, dYear1, nOvipositing) is called. */
    vecDay<double> nOvipositingDelayed;
};

}
}
}
#endif
