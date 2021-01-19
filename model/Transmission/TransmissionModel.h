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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,
 * USA.
 */

#ifndef Hmod_TransmissionModel
#define Hmod_TransmissionModel

#include <string.h>

#include <fstream>

#include "Global.h"
#include "schema/interventions.h"
#include "util/errors.h"

#include "Transmission/PerHost.h"
#include "Population.h"
#include "WithinHost/WHInterface.h"
#include "WithinHost/Genotypes.h"
#include "mon/Continuous.h"
#include "mon/info.h"
#include "util/StreamValidator.h"
#include "util/CommandLine.h"
#include "util/vectors.h"
#include "util/ModelOptions.h"

#include <cmath>
#include <cfloat>
#include <gsl/gsl_vector.h>

namespace OM
{
class Summary;
class Population;
namespace Host
{
class Human;
}
namespace Transmission
{
class PerHost;

/** Variable describing current simulation mode. */
enum SimulationMode
{
    /** Initial mode. Indicates that initialisation still needs to happen (i.e.
     * it is an error if this mode is still set when getEIR is called). */
    preInit,
    /** Equilibrium mode (i.e. just apply a fixed EIR)
     *
     * This is used for the warm-up period and if we want to separate direct
     * effect of an intervention from indirect effects via transmission
     * intensity. The seasonal pattern and intensity of the EIR do not change
     * over years.
     *
     * In this mode vector calculations aren't run. */
    forcedEIR,
    /** Transient EIR known
     *
     * This is used to simulate an intervention that changes EIR, and where we
     * have measurements of the EIR over time during the intervention period.
     */
    transientEIRknown,
    /** EIR changes
     *
     * The simulation is driven by the EIR which changes dynamically during the
     * intervention phase as a function of the characteristics of the
     * interventions.
     *
     * Dependending on whether the Vector or NonVector model is in use, this
     * EIR may be calculated from a mosquito emergence rate or be an input EIR
     * scaled by the relative infectiousness of the humans. */
    dynamicEIR,
};

static SimulationMode readMode(const string &str)
{
    if (str == "forced")
        return forcedEIR;
    else if (str == "dynamic")
        return dynamicEIR;
    else
        // Note: originally 3 (transientEIRknown) could be specified; now it's
        // set automatically.
        throw util::xml_scenario_error(string("mode attribute invalid: ").append(str));
}

/// Abstract base class, defines behaviour of transmission models
class TransmissionModel
{
protected:
    /// Reads all entomological parameters from the input datafile.
    /// @param entoData input configuration for model
    /// @param nGenotypes number of genotypes transmission model is using
    TransmissionModel(const scnXml::Entomology &entoData, size_t nGenotypes)
        : simulationMode(forcedEIR)
        , interventionMode(readMode(entoData.getMode()))
        , laggedKappa(1, 0.0)
        , // if using non-vector model, it will resize this
        annualEIR(0.0)
        , _annualAverageKappa(numeric_limits<double>::signaling_NaN())
        , _sumAnnualKappa(0.0)
        , tsAdultEIR(0.0)
        , surveyInputEIR(0.0)
        , surveySimulatedEIR(0.0)
        , adultAge(PerHost::adultAge())
        , numTransmittingHumans(0)
    {
        initialisationEIR.assign(sim::stepsPerYear(), 0.0);

        using mon::Continuous;
        Continuous.registerCallback("input EIR", "\tinput EIR", MakeDelegate(this, &TransmissionModel::ctsCbInputEIR));
        Continuous.registerCallback("simulated EIR", "\tsimulated EIR", MakeDelegate(this, &TransmissionModel::ctsCbSimulatedEIR));
        Continuous.registerCallback("human infectiousness", "\thuman infectiousness", MakeDelegate(this, &TransmissionModel::ctsCbKappa));
        Continuous.registerCallback("num transmitting humans", "\tnum transmitting humans",
                                    MakeDelegate(this, &TransmissionModel::ctsCbNumTransmittingHumans));
    }

public:
    //! Deallocate memory for TransmissionModel parameters and clean up
    virtual ~TransmissionModel() {}

    /** Extra initialisation when not loading from a checkpoint, requiring
     * information from the human population structure. */
    virtual void init2(const Population &population) = 0;

    /** Set up vector population interventions. */
    virtual void initVectorInterv(const scnXml::Description::AnophelesSequence &list, size_t instance, const string &name) = 0;

    /** Set up vector trap interventions. */
    virtual void initVectorTrap(const scnXml::VectorTrap::DescriptionSequence list, size_t instance,
                                const scnXml::VectorTrap::NameOptional name) = 0;

    // /** Set up non-human hosts interventions. */
    virtual void initNonHumanHostsInterv(const scnXml::Description2::AnophelesSequence list, const scnXml::DecayFunction &decay,
                                         size_t instance, const string &name) = 0;

    // /** Set up non-human hosts interventions. */
    virtual void initAddNonHumanHostsInterv(const scnXml::Description3::AnophelesSequence list, const string &name) = 0;

    /// Checkpointing
    template <class S>
    void operator&(S &stream)
    {
        checkpoint(stream);
    }
    //@}

    /** Set some summary items.
     *
     * Overriding functions should call this base version too. */
    virtual void summarize()
    {
        mon::reportStatMF(mon::MVF_NUM_TRANSMIT, laggedKappa[sim::now().moduloSteps(laggedKappa.size())]);
        mon::reportStatMF(mon::MVF_ANN_AVG_K, _annualAverageKappa);

        if (!mon::isReported()) return; // cannot use counters below when not reporting

        double duration = (sim::now() - lastSurveyTime).inSteps();
        if (duration > 0.0)
        {
            mon::reportStatMF(mon::MVF_INPUT_EIR, surveyInputEIR / duration);
            mon::reportStatMF(mon::MVF_SIM_EIR, surveySimulatedEIR / duration);
        }

        surveyInputEIR = 0.0;
        surveySimulatedEIR = 0.0;
        lastSurveyTime = sim::now();
    }

    /** Scale the EIR used by the model.
     *
     * EIR is scaled in memory (so will affect this simulation).
     * XML data is not touched. */
    virtual void scaleEIR(double factor) = 0;

#if 0
  /** Scale the EIR descriptions in the XML element.
   * This updates the XML, and not the EIR descriptions used for simulations.
   * In order for changes to be written back to the XML file,
   * InputData.documentChanged needs to be set.
   * 
   * @param ed	Access to XML element to update.
   * @param factor	Multiplicative factor by which to scale EIR. */
  virtual void scaleXML_EIR (scnXml::EntoData& ed, double factor) const =0;
#endif

    /** How many intervals are needed for transmission initialization during the
     * "human" phase (before vector init)?
     *
     * Should include time for both data collection and to give the data
     * collected time to stabilize. */
    virtual SimTime minPreinitDuration() = 0;
    /** Length of time that initIterate() is most likely to add: only used to
     * estimate total runtime. */
    virtual SimTime expectedInitDuration() = 0;
    /** Check whether transmission has been sufficiently well initialized. If so,
     * switch to dynamic transmission mode. If not, try to improve the situation
     * and return the length of sim-time before this should be called again.
     */
    virtual SimTime initIterate() = 0;

    /** Needs to be called each step of the simulation before Human::update().
     *
     * when the vector model is used this updates mosquito populations. */
    virtual void vectorUpdate(const Population &population){};
    /** Needs to be called each time-step after Human::update().
     *
     * Updates summary statistics related to transmission as well as the
     * the non-vector model (when in use). */
    virtual void update(const Population &population) = 0;

    virtual void changeEIRIntervention(const scnXml::NonVector &)
    {
        throw util::xml_scenario_error("changeEIR intervention can only be used with NonVectorModel!");
    }

    /** Does per-time-step updates and returns the EIR (inoculation rate per host
     * per time step). Should be called exactly once per time-step (at least,
     * during the intervention period when ITNs may be in use).
     *
     * Non-vector:
     * During the pre-intervention phase, the EIR is forced, using values from
     * the XML file. During the main simulation phase, it may be calculated or
     * obtained from data in the XML file.
     *
     * Vector:
     * During vector initialisation phase, EIR is forced based on the EIR given
     * in the XML file as a Fourier Series. After endVectorInitPeriod() is called
     * the simulation switches to using dynamic EIR. advanceStep _must_ be
     * called before this function in order to return the correct value.
     *
     * @param human A reference to the human who's EIR is being calculated.
     *    The human's "per host transmission" potentially needs updating.
     * @param age Age of the human in time units
     * @param ageYears Age of the human in years
     * @param EIR Out-vector of EIR per parasite genotype. The length is also set
     *    by the called function. Where genotype tracking is not supported (e.g.
     *    the non-vector model), the length is set to one.
     * @returns the sum of EIR across genotypes
     */
    double getEIR(Host::Human &human, SimTime age, double ageYears, vector<double> &EIR)
    {
        /* For the NonVector model, the EIR should just be multiplied by the
         * availability. For the Vector model, the availability is also required
         * for internal calculations, but again the EIR should be multiplied by the
         * availability. */
        calculateEIR(human, ageYears, EIR);
        util::streamValidate(EIR);

        double allEIR = util::vectors::sum(EIR);
        if (age >= adultAge)
        {
            tsAdultEntoInocs += allEIR;
            tsNumAdults += 1;
        }
        return allEIR;
    }

    /** Deploy a vector population intervention.
     *
     * Instance: the index of this instance of the intervention. Each instance
     * has it's own parameterisation. 0 <= instance < N where N is the number of
     * instances. */
    virtual void deployVectorPopInterv(size_t instance) = 0;
    /// Deploy some vector traps.
    ///
    /// @param instance Index of this type of trap
    /// @param number The number of traps to deploy
    /// @param lifespan Time until these traps are removed/replaced/useless
    virtual void deployVectorTrap(size_t instance, double popSize, SimTime lifespan) = 0;

    virtual void deployNonHumanHostsInterv(size_t instance, string name) = 0;

    virtual void deployAddNonHumanHosts(string name, double popSize, SimTime lifespan) = 0;

    /** Remove all current infections to mosquitoes, such that without re-
     * infection, humans will then be exposed to zero EIR. */
    virtual void uninfectVectors() = 0;

protected:
    /** Calculates the EIR individuals are exposed to.
     *
     * Call once per time-step: updates ITNs in vector model.
     *
     * @param host Transmission data for the human to calculate EIR for.
     * @param ageGroupData Age group of this host for availablility data.
     * @param EIR Out-vector. Set to the age- and heterogeneity-specific EIR an
     *    individual human is exposed to, per parasite genotype, in units of
     *    inoculations per day. Length set by callee. */
    virtual void calculateEIR(Host::Human &human, double ageYears, vector<double> &EIR) const = 0;

    /** Needs to be called each time-step after Human::update() to update summary
     * statististics related to transmission. Also returns kappa (the average
     * human infectiousness weighted by availability to mosquitoes). */
    double updateKappa(const Population &population)
    {
        // We calculate kappa for output and the non-vector model.
        double sumWt_kappa = 0.0;
        double sumWeight = 0.0;
        numTransmittingHumans = 0;

        for (const Host::Human &human : population.getHumans())
        {
            // NOTE: calculate availability relative to age at end of time step;
            // not my preference but consistent with TransmissionModel::getEIR().
            const double avail = human.perHostTransmission.relativeAvailabilityHetAge(human.age(sim::ts1()).inYears());
            sumWeight += avail;
            const double tbvFactor = human.getVaccine().getFactor(interventions::Vaccine::TBV);
            const double pTransmit = human.withinHostModel->probTransmissionToMosquito(tbvFactor, 0);
            const double riskTrans = avail * pTransmit;
            sumWt_kappa += riskTrans;
            if (riskTrans > 0.0) ++numTransmittingHumans;
        }

        size_t lKMod = sim::ts1().moduloSteps(laggedKappa.size()); // now
        if (population.size() == 0)
        {                             // this is valid
            laggedKappa[lKMod] = 0.0; // no humans: no infectiousness
        }
        else
        {
            if (!(sumWeight > DBL_MIN * 10.0))
            { // if approx. eq. 0, negative or an NaN
                ostringstream msg;
                msg << "sumWeight is invalid: " << sumWeight << ", " << sumWt_kappa << ", " << population.size();
                throw TRACED_EXCEPTION(msg.str(), util::Error::SumWeight);
            }
            laggedKappa[lKMod] = sumWt_kappa / sumWeight;
        }

        size_t tmod = sim::ts0().moduloYearSteps();

        // Calculate time-weighted average of kappa
        _sumAnnualKappa += laggedKappa[lKMod] * initialisationEIR[tmod];
        if (tmod == sim::stepsPerYear() - 1)
        {
            _annualAverageKappa = _sumAnnualKappa / annualEIR; // inf or NaN when annualEIR is 0
            _sumAnnualKappa = 0.0;
        }

        tsAdultEIR = tsAdultEntoInocs / tsNumAdults;
        tsAdultEntoInocs = 0.0;
        tsNumAdults = 0;

        surveyInputEIR += initialisationEIR[tmod];
        surveySimulatedEIR += tsAdultEIR;

        return laggedKappa[lKMod]; // kappa now
    }

    virtual void checkpoint(istream &stream)
    {
        simulationMode &stream;
        interventionMode &stream;
        initialisationEIR &stream;
        laggedKappa &stream;
        annualEIR &stream;
        _annualAverageKappa &stream;
        _sumAnnualKappa &stream;
        tsAdultEIR &stream;
        surveyInputEIR &stream;
        surveySimulatedEIR &stream;
        lastSurveyTime &stream;
        adultAge &stream;
        numTransmittingHumans &stream;
    }

    virtual void checkpoint(ostream &stream)
    {
        simulationMode &stream;
        interventionMode &stream;
        initialisationEIR &stream;
        laggedKappa &stream;
        annualEIR &stream;
        _annualAverageKappa &stream;
        _sumAnnualKappa &stream;
        tsAdultEIR &stream;
        surveyInputEIR &stream;
        surveySimulatedEIR &stream;
        lastSurveyTime &stream;
        adultAge &stream;
        numTransmittingHumans &stream;
    }

private:
    // The times here should be for the last updated index of arrays:
    void ctsCbInputEIR(ostream &stream)
    {
        int prevStep = (sim::now() - SimTime::oneTS()) / SimTime::oneTS();
        // Note: prevStep may be negative, hence util::mod not mod_nn:
        stream << '\t' << initialisationEIR[util::mod(prevStep, sim::stepsPerYear())];
    }
    void ctsCbSimulatedEIR(ostream &stream) { stream << '\t' << tsAdultEIR; }
    void ctsCbKappa(ostream &stream)
    {
        // The latest time-step's kappa:
        stream << '\t' << laggedKappa[sim::now().moduloSteps(laggedKappa.size())];
    }
    void ctsCbNumTransmittingHumans(ostream &stream) { stream << '\t' << numTransmittingHumans; }

public:
    /** The type of EIR calculation. Checkpointed. */
    int simulationMode;
    /** New simulation mode during intervention period. Not checkpointed. */
    int interventionMode;

    /** Entomological inoculation rate for adults during the
     * pre-intervention phase.
     *
     * Length: time-steps per year
     *
     * Index sim::now_mod_steps_per_year() corresponds to the EIR
     * acting on the current time-step: i.e. total inoculations since the
     * previous time-step.
     * Since time-step 0 is not calculated, initialisationEIR[0] is actually the
     * last value used (to calculate the state at the start of the second year).
     *
     * Units: infectious bites per adult per time step
     *
     * Not checkpointed; doesn't need to be except when a changeEIR intervention
     * occurs. */
    vector<double> initialisationEIR;

    /** The probability of infection of a mosquito at each bite.
     * It is calculated as the average infectiousness per human.
     *
     * The value in index sim::ts1().moduloSteps(initialKappa.size()) is the
     * kappa from this time step (i.e. the infectiousness of humans at the end of
     * this step). Length depends on entomological incubation period from
     * non-vector model.
     *
     * Checkpointed. */
    vector<double> laggedKappa;

    /** Total annual infectious bites per adult.
     *
     * Checkpointed. */
    double annualEIR;

private:
    /*! annAvgKappa is the overall proportion of mosquitoes that get infected
     * allowing for the different densities in different seasons (approximating
     * relative mosquito density with the EIR).
     *
     * Checkpointed. */
    double _annualAverageKappa;

    /*! Used to calculate annAvgKappa.
     *
     * Checkpointed. */
    double _sumAnnualKappa;

    /// Adult-only EIR over the last update
    double tsAdultEIR;

    /** Per-time-step input EIR summed over inter-survey period.
     * Units: infectious bites/adult/inter-survey period. */
    double surveyInputEIR;
    /** Per-time-step simulated EIR summed over inter-survey period.
     * Units: infectious bites/adult/inter-survey period. */
    double surveySimulatedEIR;
    /** Time of last survey. */
    SimTime lastSurveyTime;

    /// age at which an individual is considered an adult
    SimTime adultAge;

    /// For "num transmitting humans" cts output.
    int numTransmittingHumans;

    // Reporting data. Doesn't need checkpointing due to reset every time-step.
    double tsAdultEntoInocs = 0.0;  // accumulator for time step EIR of adults
    int tsNumAdults = 0; // accumulator for time step adults requesting EIR
};

} // namespace Transmission
} // namespace OM
#endif
