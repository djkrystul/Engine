/*
 Copyright (C) 2018 Quaternion Risk Management Ltd
 All rights reserved.

 This file is part of ORE, a free-software/open-source library
 for transparent pricing and risk analysis - http://opensourcerisk.org

 ORE is free software: you can redistribute it and/or modify it
 under the terms of the Modified BSD License.  You should have received a
 copy of the license along with this program.
 The license is also available online at <http://opensourcerisk.org>

 This program is distributed on the basis that it will form a useful
 contribution to risk analytics and model standardisation, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 FITNESS FOR A PARTICULAR PURPOSE. See the license for more details.
*/

#include <orea/simm/crifrecord.hpp>
#include <orea/simm/crifloader.hpp>
#include <orea/simm/simmcalculator.hpp>
#include <orea/simm/utilities.hpp>

#include <boost/math/distributions/normal.hpp>
#include <numeric>
#include <ored/portfolio/structuredtradewarning.hpp>
#include <ored/utilities/log.hpp>
#include <ored/utilities/to_string.hpp>
#include <ored/utilities/parsers.hpp>
#include <ql/math/comparison.hpp>
#include <ql/quote.hpp>

using std::abs;
using std::accumulate;
using std::make_pair;
using std::make_tuple;
using std::map;
using std::max;
using std::min;
using std::pair;
using std::set;
using std::sqrt;
using std::string;

using ore::data::checkCurrency;
using ore::data::NettingSetDetails;
using ore::data::Market;
using ore::data::to_string;
using ore::data::parseBool;
using QuantLib::close_enough;
using QuantLib::Real;

namespace ore {
namespace analytics {

// Ease notation again
typedef SimmConfiguration::ProductClass ProductClass;
typedef SimmConfiguration::RiskClass RiskClass;
typedef SimmConfiguration::MarginType MarginType;
typedef SimmConfiguration::RiskType RiskType;
typedef SimmConfiguration::Regulation Regulation;
typedef SimmConfiguration::SimmSide SimmSide;

SimmCalculator::SimmCalculator(const SimmNetSensitivities& simmNetSensitivities,
                               const boost::shared_ptr<SimmConfiguration>& simmConfiguration,
                               const string& calculationCcy, const string& resultCcy, const boost::shared_ptr<Market> market,
                               const bool determineWinningRegulations,
                               const bool enforceIMRegulations, const bool quiet)
    : simmNetSensitivities_(simmNetSensitivities), simmConfiguration_(simmConfiguration),
      calculationCcy_(calculationCcy), resultCcy_(resultCcy.empty() ? calculationCcy_ : resultCcy), market_(market),
      quiet_(quiet) {

    QL_REQUIRE(checkCurrency(calculationCcy_),
               "SIMM Calculator: The calculation currency (" << calculationCcy_ << ") must be a valid ISO currency code");
    QL_REQUIRE(checkCurrency(resultCcy_),
               "SIMM Calculator: The result currency (" << resultCcy_ << ") must be a valid ISO currency code");

    SimmNetSensitivities tmp;
    for (const CrifRecord& cr : simmNetSensitivities_) {
        // Remove Schedule-only CRIF records
        const bool isSchedule = cr.imModel == "Schedule";
        if (isSchedule) {
            if (!quiet_ && determineWinningRegulations) {
                WLOG(ore::data::StructuredTradeWarningMessage(cr.tradeId, cr.tradeType, "SIMM calculator", "Skipping over Schedule CRIF record"));
            } 
            continue;
        }

        // Check for each netting set whether post/collect regs are populated at all
        if (collectRegsIsEmpty_.find(cr.nettingSetDetails) == collectRegsIsEmpty_.end()) {
            collectRegsIsEmpty_[cr.nettingSetDetails] = cr.collectRegulations.empty();
        } else if (collectRegsIsEmpty_.at(cr.nettingSetDetails) && !cr.collectRegulations.empty()) {
            collectRegsIsEmpty_.at(cr.nettingSetDetails) = false;
        }
        if (postRegsIsEmpty_.find(cr.nettingSetDetails) == postRegsIsEmpty_.end()) {
            postRegsIsEmpty_[cr.nettingSetDetails] = cr.postRegulations.empty();
        } else if (postRegsIsEmpty_.at(cr.nettingSetDetails) && !cr.postRegulations.empty()) {
            postRegsIsEmpty_.at(cr.nettingSetDetails) = false;
        }

        tmp.insert(cr);
    }

    // If there are no CRIF records to process
    if (tmp.empty())
        return;
    
    simmNetSensitivities_ = tmp;

    // Add CRIF records to each regulation under each netting set
    if (!quiet_) {
        LOG("SimmCalculator: Splitting up original CRIF records into their respective collect/post regulations");
    }
    for (const CrifRecord& crifRecord : simmNetSensitivities_) {
        addCrifRecord(crifRecord, SimmSide::Call, enforceIMRegulations);
        addCrifRecord(crifRecord, SimmSide::Post, enforceIMRegulations);
    }

    // Some additional processing depending on the regulations applicable to each netting set
    for (auto& sv : regSensitivities_) {

        for (auto& s : sv.second) {
            // Where there is SEC and CFTC in the portfolio, we add the CFTC trades to SEC,
            // but still continue with CFTC calculations
            const bool hasCFTC = s.second.count("CFTC") > 0;
            const bool hasSEC = s.second.count("SEC") > 0;
            if (hasCFTC && hasSEC) {
                const SimmNetSensitivities& netRecordsCFTC = s.second.at("CFTC")->netRecords(true);
                const SimmNetSensitivities& netRecordsSEC = s.second.at("SEC")->netRecords(true);
                for (const auto& cr : netRecordsCFTC) {
                    // Only add CFTC records to SEC if the record was not already in SEC,
                    // i.e. we skip over CRIF records with regulations specified as e.g. "..., CFTC, SEC, ..."
                    if (netRecordsSEC.find(cr) == netRecordsSEC.end()) {
                        if (!quiet_) {
                            DLOG("SimmCalculator: Inserting CRIF record with CFTC "
                                << s.first << " regulation into SEC CRIF records: " << cr);
                        }
                        s.second.at("SEC")->add(cr);
                    }
                }
            }

            // If netting set has "Unspecified" plus other regulations, the "Unspecified" sensis are to be excluded.
            // If netting set only has "Unspecified", then no regulations were ever specified, so all trades are
            // included.
            if (s.second.count("Unspecified") > 0 && s.second.size() > 1)
                s.second.erase("Unspecified");
        }
    }

    // Calculate SIMM call and post for each regulation under each netting set
    for (const auto& sv : regSensitivities_) {
        const SimmSide side = sv.first;
        for (const auto& nettingSetSensis : sv.second) {
            const NettingSetDetails& nsd = nettingSetSensis.first;

            // Calculate SIMM for particular side-nettingSet-regulation combination
            for (const auto& regSensis : nettingSetSensis.second) {
                const string& regulation = regSensis.first;
                bool hasFixedAddOn = false;
                for (const auto& sp : regSensis.second->simmParameters())
                    if (sp.riskType == RiskType::AddOnFixedAmount) {
                        hasFixedAddOn = true;
                        break;
                    }
                if (regSensis.second->hasCrifRecords() || hasFixedAddOn)
                    calculateRegulationSimm(regSensis.second->netRecords(true), nsd, regulation, side);
            }
        }
    }

    // Convert to result currency
    convert();

    // Determine winning call and post regulations
    if (determineWinningRegulations) {
        if (!quiet_) {
            LOG("SimmCalculator: Determining winning regulations");
        }

        for (auto sv : simmResults_) {
            const SimmSide side = sv.first;

            // Determine winning (call and post) regulation for each netting set
            for (const auto& kv : sv.second) {

                // Collect margin amounts and determine the highest margin amount
                Real winningMargin = std::numeric_limits<Real>::min();
                map<string, Real> nettingSetMargins;
                std::vector<Real> margins;
                for (const auto& regSimmResults : kv.second) {
                    const Real& im = regSimmResults.second.get(ProductClass::All, RiskClass::All, MarginType::All, "All");
                    nettingSetMargins[regSimmResults.first] = im;
                    if (im > winningMargin)
                        winningMargin = im;
                }

                // Determine winning regulations, i.e. regulations under which we find the highest margin amount
                std::vector<string> winningRegulations;
                for (const auto& kv : nettingSetMargins) {
                    if (close_enough(kv.second, winningMargin))
                        winningRegulations.push_back(kv.first);
                }

                // In the case of multiple winning regulations, pick one based on the priority in the list
                //const Regulation winningRegulation = oreplus::analytics::getWinningRegulation(winningRegulations);
                string winningRegulation = winningRegulations.size() > 1
                                               ? to_string(getWinningRegulation(winningRegulations))
                                               : winningRegulations.at(0);

                // Populate internal list of winning regulators
                winningRegulations_[side][kv.first] = to_string(winningRegulation);
            }
        }

        populateFinalResults();
    }
}

const void SimmCalculator::calculateRegulationSimm(const SimmNetSensitivities& netRecords,
                                                   const NettingSetDetails& nettingSetDetails, const string& regulation,
                                                   const SimmSide& side) {

    if (!quiet_) {
        LOG("SimmCalculator: Calculating SIMM " << side << " for portfolio [" << nettingSetDetails << "], regulation "
                                                << regulation);
    }

    // Index in to SimmNetSensitivities
    auto& indexProduct = netRecords.get<ProductClassTag>();

    // Loop over portfolios and product classes
    for (auto it = indexProduct.begin(); it != indexProduct.end();
         it = indexProduct.upper_bound(make_tuple(nettingSetDetails, it->productClass))) {
        auto productClass = it->productClass;
        if (!quiet_) {
            LOG("SimmCalculator: Calculating SIMM for product class " << productClass);
        }

        // Delta margin components
        RiskClass rc = RiskClass::InterestRate;
        MarginType mt = MarginType::Delta;
        auto p = irDeltaMargin(nettingSetDetails, productClass, netRecords);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        rc = RiskClass::FX;
        p = margin(nettingSetDetails, productClass, RiskType::FX, netRecords);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        rc = RiskClass::CreditQualifying;
        p = margin(nettingSetDetails, productClass, RiskType::CreditQ, netRecords);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        rc = RiskClass::CreditNonQualifying;
        p = margin(nettingSetDetails, productClass, RiskType::CreditNonQ, netRecords);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        rc = RiskClass::Equity;
        p = margin(nettingSetDetails, productClass, RiskType::Equity, netRecords);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        rc = RiskClass::Commodity;
        p = margin(nettingSetDetails, productClass, RiskType::Commodity, netRecords);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        // Vega margin components
        mt = MarginType::Vega;
        rc = RiskClass::InterestRate;
        p = irVegaMargin(nettingSetDetails, productClass, netRecords);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        rc = RiskClass::FX;
        p = margin(nettingSetDetails, productClass, RiskType::FXVol, netRecords);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        rc = RiskClass::CreditQualifying;
        p = margin(nettingSetDetails, productClass, RiskType::CreditVol, netRecords);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        rc = RiskClass::CreditNonQualifying;
        p = margin(nettingSetDetails, productClass, RiskType::CreditVolNonQ, netRecords);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        rc = RiskClass::Equity;
        p = margin(nettingSetDetails, productClass, RiskType::EquityVol, netRecords);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        rc = RiskClass::Commodity;
        p = margin(nettingSetDetails, productClass, RiskType::CommodityVol, netRecords);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        // Curvature margin components for sides call and post
        mt = MarginType::Curvature;
        rc = RiskClass::InterestRate;

        p = irCurvatureMargin(nettingSetDetails, productClass, side, netRecords);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        rc = RiskClass::FX;
        p = curvatureMargin(nettingSetDetails, productClass, RiskType::FXVol, side, netRecords, false);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        rc = RiskClass::CreditQualifying;
        p = curvatureMargin(nettingSetDetails, productClass, RiskType::CreditVol, side, netRecords);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        rc = RiskClass::CreditNonQualifying;
        p = curvatureMargin(nettingSetDetails, productClass, RiskType::CreditVolNonQ, side, netRecords);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        rc = RiskClass::Equity;
        p = curvatureMargin(nettingSetDetails, productClass, RiskType::EquityVol, side, netRecords, false);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        rc = RiskClass::Commodity;
        p = curvatureMargin(nettingSetDetails, productClass, RiskType::CommodityVol, side, netRecords, false);
        if (p.second)
            add(nettingSetDetails, regulation, productClass, rc, mt, p.first, side);

        // Base correlation margin components. This risk type came later so need to check
        // first if it is valid under the configuration
        if (simmConfiguration_->isValidRiskType(RiskType::BaseCorr)) {
            p = margin(nettingSetDetails, productClass, RiskType::BaseCorr, netRecords);
            if (p.second)
                add(nettingSetDetails, regulation, productClass, RiskClass::CreditQualifying, MarginType::BaseCorr,
                    p.first, side);
        }
    }

    // Calculate the higher level margins
    populateResults(side, nettingSetDetails, regulation);

    // For each portfolio, calculate the additional margin
    calcAddMargin(side, nettingSetDetails, regulation, netRecords);
}

const string& SimmCalculator::winningRegulations(const SimmSide& side, const NettingSetDetails& nettingSetDetails) const {
    const auto& subWinningRegs = winningRegulations(side);
    QL_REQUIRE(subWinningRegs.find(nettingSetDetails) != subWinningRegs.end(),
               "SimmCalculator::winningRegulations(): Could not find netting set in the list of "
                   << side << " IM winning regulations: " << nettingSetDetails);
    return subWinningRegs.at(nettingSetDetails);
}

const map<NettingSetDetails, string>& SimmCalculator::winningRegulations(const SimmSide& side) const {
    QL_REQUIRE(winningRegulations_.find(side) != winningRegulations_.end(),
               "SimmCalculator::winningRegulations(): Could not find list of" << side << " IM winning regulations");
    return winningRegulations_.at(side);
}

const map<SimmSide, map<NettingSetDetails, string>>& SimmCalculator::winningRegulations() const {
    return winningRegulations_;
}

const SimmResults& SimmCalculator::simmResults(const SimmSide& side, const NettingSetDetails& nettingSetDetails,
                                               const string& regulation) const {
    const auto& subResults = simmResults(side, nettingSetDetails);
    QL_REQUIRE(subResults.find(regulation) != subResults.end(),
               "SimmCalculator::simmResults(): Could not find regulation in the SIMM "
                   << side << " results for netting set [" << nettingSetDetails << "]: " << regulation);
    return subResults.at(regulation);
}

const map<string, SimmResults>& SimmCalculator::simmResults(const SimmSide& side,
                                                            const NettingSetDetails& nettingSetDetails) const {
    const auto& subResults = simmResults(side);
    QL_REQUIRE(subResults.find(nettingSetDetails) != subResults.end(),
               "SimmCalculator::simmResults(): Could not find netting set in the SIMM "
                   << side << " results: " << nettingSetDetails);
    return subResults.at(nettingSetDetails);
}

const map<NettingSetDetails, map<string, SimmResults>>& SimmCalculator::simmResults(const SimmSide& side) const {
    QL_REQUIRE(simmResults_.find(side) != simmResults_.end(),
               "SimmCalculator::simmResults(): Could not find " << side << " IM in the SIMM results");
    return simmResults_.at(side);
}

const map<SimmSide, map<NettingSetDetails, map<string, SimmResults>>>& SimmCalculator::simmResults() const {
    return simmResults_;
}

const pair<string, SimmResults>& SimmCalculator::finalSimmResults(const SimmSide& side,
                                                                  const NettingSetDetails& nettingSetDetails) const {
    const auto& subResults = finalSimmResults(side);
    QL_REQUIRE(subResults.find(nettingSetDetails) != subResults.end(),
               "SimmCalculator::finalSimmResults(): Could not find netting set in the final SIMM "
                   << side << " results: " << nettingSetDetails);
    return subResults.at(nettingSetDetails);
}

const map<NettingSetDetails, pair<string, SimmResults>>& SimmCalculator::finalSimmResults(const SimmSide& side) const {
    QL_REQUIRE(finalSimmResults_.find(side) != finalSimmResults_.end(),
               "SimmCalculator::finalSimmResults(): Could not find " << side << " IM in the final SIMM results");
    return finalSimmResults_.at(side);
}

const map<SimmSide, map<NettingSetDetails, pair<string, SimmResults>>>& SimmCalculator::finalSimmResults() const {
    return finalSimmResults_;
}

pair<map<string, Real>, bool> SimmCalculator::irDeltaMargin(const NettingSetDetails& nettingSetDetails,
                                                            const ProductClass& pc,
                                                            const SimmNetSensitivities& netRecords) const {

    // "Bucket" here referse to exposures under the CRIF qualifiers
    map<string, Real> bucketMargins;

    // Index on SIMM sensitivities in to Risk Type level
    auto& ssRiskTypeIndex = netRecords.get<RiskTypeTag>();
    // Index on SIMM sensitivities in to Qualifier level
    auto& ssQualifierIndex = netRecords.get<QualifierTag>();

    // Find the set of qualifiers, i.e. currencies, in the Simm sensitivities
    set<string> qualifiers;

    // IRCurve qualifiers
    auto pIrCurve = ssRiskTypeIndex.equal_range(make_tuple(nettingSetDetails, pc, RiskType::IRCurve));
    while (pIrCurve.first != pIrCurve.second) {
        if (qualifiers.count(pIrCurve.first->qualifier) == 0) {
            qualifiers.insert(pIrCurve.first->qualifier);
        }
        ++pIrCurve.first;
    }
    // XCcyBasis qualifiers
    auto pXccy = ssRiskTypeIndex.equal_range(make_tuple(nettingSetDetails, pc, RiskType::XCcyBasis));
    while (pXccy.first != pXccy.second) {
        if (qualifiers.count(pXccy.first->qualifier) == 0) {
            qualifiers.insert(pXccy.first->qualifier);
        }
        ++pXccy.first;
    }
    // Inflation qualifiers
    auto pInflation = ssRiskTypeIndex.equal_range(make_tuple(nettingSetDetails, pc, RiskType::Inflation));
    while (pInflation.first != pInflation.second) {
        if (qualifiers.count(pInflation.first->qualifier) == 0) {
            qualifiers.insert(pInflation.first->qualifier);
        }
        ++pInflation.first;
    }

    // If there are no qualifiers, return early and set bool to false to indicate margin does not apply
    if (qualifiers.empty()) {
        bucketMargins["All"] = 0.0;
        return make_pair(bucketMargins, false);
    }

    // Hold the concentration risk for each qualifier i.e. $CR_b$ from SIMM docs
    map<string, Real> concentrationRisk;
    // The delta margin for each currency i.e. $K_b$ from SIMM docs
    map<string, Real> deltaMargin;
    // The sum of the weighted sensitivities for each currency i.e. $\sum_{i,k} WS_{k,i}$ from SIMM docs
    map<string, Real> sumWeightedSensis;

    // Loop over the qualifiers i.e. currencies
    for (const auto& qualifier : qualifiers) {
        // Pair of iterators to start and end of IRCurve sensitivities with current qualifier
        auto pIrQualifier = ssQualifierIndex.equal_range(make_tuple(nettingSetDetails, pc, RiskType::IRCurve, qualifier));

        // Iterator to Xccy basis element with current qualifier (expect zero or one element)
        auto XccyCount = ssQualifierIndex.count(make_tuple(nettingSetDetails, pc, RiskType::XCcyBasis, qualifier));
        QL_REQUIRE(XccyCount < 2, "SIMM Calcuator: Expected either 0 or 1 elements for risk type "
                                      << RiskType::XCcyBasis << " and qualifier " << qualifier << " but got "
                                      << XccyCount);
        auto itXccy = ssQualifierIndex.find(make_tuple(nettingSetDetails, pc, RiskType::XCcyBasis, qualifier));

        // Iterator to inflation element with current qualifier (expect zero or one element)
        auto inflationCount = ssQualifierIndex.count(make_tuple(nettingSetDetails, pc, RiskType::Inflation, qualifier));
        QL_REQUIRE(inflationCount < 2, "SIMM Calculator: Expected either 0 or 1 elements for risk type "
                                           << RiskType::Inflation << " and qualifier " << qualifier << " but got "
                                           << inflationCount);
        auto itInflation = ssQualifierIndex.find(make_tuple(nettingSetDetails, pc, RiskType::Inflation, qualifier));

        // One pass to get the concentration risk for this qualifier
        // Note: XccyBasis is not included in the calculation of concentration risk and the XccyBasis sensitivity
        //       is not scaled by it
        for (auto it = pIrQualifier.first; it != pIrQualifier.second; ++it) {
            concentrationRisk[qualifier] += it->amountUsd;
        }
        // Add inflation sensitivity to the concentration risk
        if (itInflation != ssQualifierIndex.end()) {
            concentrationRisk[qualifier] += itInflation->amountUsd;
        }
        // Divide by the concentration risk threshold
        concentrationRisk[qualifier] /= simmConfiguration_->concentrationThreshold(RiskType::IRCurve, qualifier);
        // Final concentration risk amount
        concentrationRisk[qualifier] = max(1.0, sqrt(std::abs(concentrationRisk[qualifier])));

        // Calculate the delta margin piece for this qualifier i.e. $K_b$ from SIMM docs
        for (auto itOuter = pIrQualifier.first; itOuter != pIrQualifier.second; ++itOuter) {
            // Risk weight i.e. $RW_k$ from SIMM docs
            Real rwOuter = simmConfiguration_->weight(RiskType::IRCurve, qualifier, itOuter->label1);
            // Weighted sensitivity i.e. $WS_{k,i}$ from SIMM docs
            Real wsOuter = rwOuter * itOuter->amountUsd * concentrationRisk[qualifier];
            // Update weighted sensitivity sum
            sumWeightedSensis[qualifier] += wsOuter;
            // Add diagonal element to delta margin
            deltaMargin[qualifier] += wsOuter * wsOuter;
            // Add the cross elements to the delta margin
            for (auto itInner = pIrQualifier.first; itInner != itOuter; ++itInner) {
                // Label2 level correlation i.e. $\phi_{i,j}$ from SIMM docs
                Real subCurveCorr = simmConfiguration_->correlation(RiskType::IRCurve, qualifier, "", itOuter->label2,
                                                                    RiskType::IRCurve, qualifier, "", itInner->label2);
                // Label1 level correlation i.e. $\rho_{k,l}$ from SIMM docs
                Real tenorCorr = simmConfiguration_->correlation(RiskType::IRCurve, qualifier, itOuter->label1, "",
                                                                 RiskType::IRCurve, qualifier, itInner->label1, "");
                // Add cross element to delta margin
                Real rwInner = simmConfiguration_->weight(RiskType::IRCurve, qualifier, itInner->label1);
                Real wsInner = rwInner * itInner->amountUsd * concentrationRisk[qualifier];
                deltaMargin[qualifier] += 2 * subCurveCorr * tenorCorr * wsOuter * wsInner;
            }
        }

        // Add the Inflation component, if any
        Real wsInflation = 0.0;
        if (itInflation != ssQualifierIndex.end()) {
            // Risk weight
            Real rwInflation = simmConfiguration_->weight(RiskType::Inflation, qualifier, itInflation->label1);
            // Weighted sensitivity
            wsInflation = rwInflation * itInflation->amountUsd * concentrationRisk[qualifier];
            // Update weighted sensitivity sum
            sumWeightedSensis[qualifier] += wsInflation;
            // Add diagonal element to delta margin
            deltaMargin[qualifier] += wsInflation * wsInflation;
            // Add the cross elements (Inflation with IRCurve tenors) to the delta margin
            // Correlation (know that Label1 and Label2 do not matter)
            Real corr = simmConfiguration_->correlation(RiskType::IRCurve, qualifier, "", "", RiskType::Inflation,
                                                        qualifier, "", "");
            for (auto it = pIrQualifier.first; it != pIrQualifier.second; ++it) {
                // Add cross element to delta margin
                Real rw = simmConfiguration_->weight(RiskType::IRCurve, qualifier, it->label1);
                Real ws = rw * it->amountUsd * concentrationRisk[qualifier];
                deltaMargin[qualifier] += 2 * corr * ws * wsInflation;
            }
        }

        // Add the XccyBasis component, if any
        if (itXccy != ssQualifierIndex.end()) {
            // Risk weight
            Real rwXccy = simmConfiguration_->weight(RiskType::XCcyBasis, qualifier, itXccy->label1);
            // Weighted sensitivity (no concentration risk here)
            Real wsXccy = rwXccy * itXccy->amountUsd;
            // Update weighted sensitivity sum
            sumWeightedSensis[qualifier] += wsXccy;
            // Add diagonal element to delta margin
            deltaMargin[qualifier] += wsXccy * wsXccy;
            // Add the cross elements (XccyBasis with IRCurve tenors) to the delta margin
            // Correlation (know that Label1 and Label2 do not matter)
            Real corr = simmConfiguration_->correlation(RiskType::IRCurve, qualifier, "", "", RiskType::XCcyBasis,
                                                        qualifier, "", "");
            for (auto it = pIrQualifier.first; it != pIrQualifier.second; ++it) {
                // Add cross element to delta margin
                Real rw = simmConfiguration_->weight(RiskType::IRCurve, qualifier, it->label1, calculationCcy_);
                Real ws = rw * it->amountUsd * concentrationRisk[qualifier];
                deltaMargin[qualifier] += 2 * corr * ws * wsXccy;
            }

            // Inflation vs. XccyBasis cross component if any
            if (itInflation != ssQualifierIndex.end()) {
                // Correlation (know that Label1 and Label2 do not matter)
                Real corr = simmConfiguration_->correlation(RiskType::Inflation, qualifier, "", "", RiskType::XCcyBasis,
                                                            qualifier, "", "");
                deltaMargin[qualifier] += 2 * corr * wsInflation * wsXccy;
            }
        }

        // Finally have the value of $K_b$
        deltaMargin[qualifier] = sqrt(max(deltaMargin[qualifier], 0.0));
    }

    // Now calculate final IR delta margin by aggregating across currencies
    Real margin = 0.0;
    for (auto itOuter = qualifiers.begin(); itOuter != qualifiers.end(); ++itOuter) {
        // Diagonal term
        margin += deltaMargin.at(*itOuter) * deltaMargin.at(*itOuter);
        // Cross terms
        Real sOuter = max(min(sumWeightedSensis.at(*itOuter), deltaMargin.at(*itOuter)), -deltaMargin.at(*itOuter));
        for (auto itInner = qualifiers.begin(); itInner != itOuter; ++itInner) {
            Real sInner = max(min(sumWeightedSensis.at(*itInner), deltaMargin.at(*itInner)), -deltaMargin.at(*itInner));
            Real g = min(concentrationRisk.at(*itOuter), concentrationRisk.at(*itInner)) /
                     max(concentrationRisk.at(*itOuter), concentrationRisk.at(*itInner));
            Real corr = simmConfiguration_->correlation(RiskType::IRCurve, *itOuter, "", "", RiskType::IRCurve,
                                                        *itInner, "", "");
            margin += 2.0 * sOuter * sInner * corr * g;
        }
    }
    margin = sqrt(max(margin, 0.0));

    for (const auto& m : deltaMargin)
        bucketMargins[m.first] = m.second;
    bucketMargins["All"] = margin;

    return make_pair(bucketMargins, true);
}

pair<map<string, Real>, bool> SimmCalculator::irVegaMargin(const NettingSetDetails& nettingSetDetails,
                                                           const SimmConfiguration::ProductClass& pc,
                                                           const SimmNetSensitivities& netRecords) const {

    // "Bucket" here refers to exposures under the CRIF qualifiers
    map<string, Real> bucketMargins;

    // Index on SIMM sensitivities in to Risk Type level
    auto& ssRiskTypeIndex = netRecords.get<RiskTypeTag>();
    // Index on SIMM sensitivities in to Qualifier level
    auto& ssQualifierIndex = netRecords.get<QualifierTag>();

    // Find the set of qualifiers, i.e. currencies, in the Simm sensitivities
    set<string> qualifiers;

    // IRVol qualifiers
    auto pIrVol = ssRiskTypeIndex.equal_range(make_tuple(nettingSetDetails, pc, RiskType::IRVol));
    while (pIrVol.first != pIrVol.second) {
        if (qualifiers.count(pIrVol.first->qualifier) == 0) {
            qualifiers.insert(pIrVol.first->qualifier);
        }
        ++pIrVol.first;
    }
    // Inflation volatility qualifiers
    auto pInfVol = ssRiskTypeIndex.equal_range(make_tuple(nettingSetDetails, pc, RiskType::InflationVol));
    while (pInfVol.first != pInfVol.second) {
        if (qualifiers.count(pInfVol.first->qualifier) == 0) {
            qualifiers.insert(pInfVol.first->qualifier);
        }
        ++pInfVol.first;
    }

    // If there are no qualifiers, return early and set bool to false to indicate margin does not apply
    if (qualifiers.empty()) {
        bucketMargins["All"] = 0.0;
        return make_pair(bucketMargins, false);
    }

    // Hold the concentration risk for each qualifier i.e. $VCR_b$ from SIMM docs
    map<string, Real> concentrationRisk;
    // The vega margin for each currency i.e. $K_b$ from SIMM docs
    map<string, Real> vegaMargin;
    // The sum of the weighted sensitivities for each currency i.e. $\sum_{k=1}^K VR_{k}$ from SIMM docs
    map<string, Real> sumWeightedSensis;

    // Loop over the qualifiers i.e. currencies
    for (const auto& qualifier : qualifiers) {
        // Pair of iterators to start and end of IRVol sensitivities with current qualifier
        auto pIrQualifier = ssQualifierIndex.equal_range(make_tuple(nettingSetDetails, pc, RiskType::IRVol, qualifier));

        // Pair of iterators to start and end of InflationVol sensitivities with current qualifier
        auto pInfQualifier =
            ssQualifierIndex.equal_range(make_tuple(nettingSetDetails, pc, RiskType::InflationVol, qualifier));

        // One pass to get the concentration risk for this qualifier
        for (auto it = pIrQualifier.first; it != pIrQualifier.second; ++it) {
            concentrationRisk[qualifier] += it->amountUsd;
        }
        for (auto it = pInfQualifier.first; it != pInfQualifier.second; ++it) {
            concentrationRisk[qualifier] += it->amountUsd;
        }
        // Divide by the concentration risk threshold
        concentrationRisk[qualifier] /= simmConfiguration_->concentrationThreshold(RiskType::IRVol, qualifier);
        // Final concentration risk amount
        concentrationRisk[qualifier] = max(1.0, sqrt(std::abs(concentrationRisk[qualifier])));

        // Calculate the margin piece for this qualifier i.e. $K_b$ from SIMM docs
        // Start with IRVol vs. IRVol components
        for (auto itOuter = pIrQualifier.first; itOuter != pIrQualifier.second; ++itOuter) {
            // Risk weight i.e. $RW_k$ from SIMM docs
            Real rwOuter = simmConfiguration_->weight(RiskType::IRVol, qualifier, itOuter->label1);
            // Weighted sensitivity i.e. $WS_{k,i}$ from SIMM docs
            Real wsOuter = rwOuter * itOuter->amountUsd * concentrationRisk[qualifier];
            // Update weighted sensitivity sum
            sumWeightedSensis[qualifier] += wsOuter;
            // Add diagonal element to vega margin
            vegaMargin[qualifier] += wsOuter * wsOuter;
            // Add the cross elements to the vega margin
            for (auto itInner = pIrQualifier.first; itInner != itOuter; ++itInner) {
                // Label1 level correlation i.e. $\rho_{k,l}$ from SIMM docs
                Real corr = simmConfiguration_->correlation(RiskType::IRVol, qualifier, itOuter->label1, "",
                                                            RiskType::IRVol, qualifier, itInner->label1, "");
                // Add cross element to vega margin
                Real rwInner = simmConfiguration_->weight(RiskType::IRVol, qualifier, itInner->label1);
                Real wsInner = rwInner * itInner->amountUsd * concentrationRisk[qualifier];
                vegaMargin[qualifier] += 2 * corr * wsOuter * wsInner;
            }
        }

        // Now deal with inflation component
        // To be generic/future-proof, assume that we don't know correlation structure. The way SIMM is
        // currently, we could just sum over the InflationVol numbers within qualifier and use this.
        for (auto itOuter = pInfQualifier.first; itOuter != pInfQualifier.second; ++itOuter) {
            // Risk weight i.e. $RW_k$ from SIMM docs
            Real rwOuter = simmConfiguration_->weight(RiskType::InflationVol, qualifier, itOuter->label1);
            // Weighted sensitivity i.e. $WS_{k,i}$ from SIMM docs
            Real wsOuter = rwOuter * itOuter->amountUsd * concentrationRisk[qualifier];
            // Update weighted sensitivity sum
            sumWeightedSensis[qualifier] += wsOuter;
            // Add diagonal element to vega margin
            vegaMargin[qualifier] += wsOuter * wsOuter;
            // Add the cross elements to the vega margin
            // Firstly, against all IRVol components
            for (auto itInner = pIrQualifier.first; itInner != pIrQualifier.second; ++itInner) {
                // Correlation i.e. $\rho_{k,l}$ from SIMM docs
                Real corr = simmConfiguration_->correlation(RiskType::InflationVol, qualifier, itOuter->label1, "",
                                                            RiskType::IRVol, qualifier, itInner->label1, "");
                // Add cross element to vega margin
                Real rwInner = simmConfiguration_->weight(RiskType::IRVol, qualifier, itInner->label1);
                Real wsInner = rwInner * itInner->amountUsd * concentrationRisk[qualifier];
                vegaMargin[qualifier] += 2 * corr * wsOuter * wsInner;
            }
            // Secondly, against all previous InflationVol components
            for (auto itInner = pInfQualifier.first; itInner != itOuter; ++itInner) {
                // Correlation i.e. $\rho_{k,l}$ from SIMM docs
                Real corr = simmConfiguration_->correlation(RiskType::InflationVol, qualifier, itOuter->label1, "",
                                                            RiskType::InflationVol, qualifier, itInner->label1, "");
                // Add cross element to vega margin
                Real rwInner = simmConfiguration_->weight(RiskType::InflationVol, qualifier, itInner->label1);
                Real wsInner = rwInner * itInner->amountUsd * concentrationRisk[qualifier];
                vegaMargin[qualifier] += 2 * corr * wsOuter * wsInner;
            }
        }

        // Finally have the value of $K_b$
        vegaMargin[qualifier] = sqrt(max(vegaMargin[qualifier], 0.0));
    }

    // Now calculate final vega margin by aggregating across currencies
    Real margin = 0.0;
    for (auto itOuter = qualifiers.begin(); itOuter != qualifiers.end(); ++itOuter) {
        // Diagonal term
        margin += vegaMargin.at(*itOuter) * vegaMargin.at(*itOuter);
        // Cross terms
        Real sOuter = max(min(sumWeightedSensis.at(*itOuter), vegaMargin.at(*itOuter)), -vegaMargin.at(*itOuter));
        for (auto itInner = qualifiers.begin(); itInner != itOuter; ++itInner) {
            Real sInner = max(min(sumWeightedSensis.at(*itInner), vegaMargin.at(*itInner)), -vegaMargin.at(*itInner));
            Real g = min(concentrationRisk.at(*itOuter), concentrationRisk.at(*itInner)) /
                     max(concentrationRisk.at(*itOuter), concentrationRisk.at(*itInner));
            Real corr = simmConfiguration_->correlation(RiskType::IRVol, *itOuter, "", "", RiskType::IRVol, *itInner,
                                                        "", "", calculationCcy_);
            margin += 2.0 * sOuter * sInner * corr * g;
        }
    }
    margin = sqrt(max(margin, 0.0));

    for (const auto& m : vegaMargin)
        bucketMargins[m.first] = m.second;
    bucketMargins["All"] = margin;

    return make_pair(bucketMargins, true);
}

pair<map<string, Real>, bool> SimmCalculator::irCurvatureMargin(const NettingSetDetails& nettingSetDetails,
                                                                const SimmConfiguration::ProductClass& pc,
                                                                const SimmSide& side,
                                                                const SimmNetSensitivities& netRecords) const {

    // "Bucket" here refers to exposures under the CRIF qualifiers
    map<string, Real> bucketMargins;

    // Multiplier for sensitivities, -1 if SIMM side is Post
    Real multiplier = side == SimmSide::Call ? 1.0 : -1.0;

    // Index on SIMM sensitivities in to Risk Type level
    auto& ssRiskTypeIndex = netRecords.get<RiskTypeTag>();
    // Index on SIMM sensitivities in to Qualifier level
    auto& ssQualifierIndex = netRecords.get<QualifierTag>();

    // Find the set of qualifiers, i.e. currencies, in the Simm sensitivities
    set<string> qualifiers;

    // IRVol qualifiers
    auto pIrVol = ssRiskTypeIndex.equal_range(make_tuple(nettingSetDetails, pc, RiskType::IRVol));
    while (pIrVol.first != pIrVol.second) {
        if (qualifiers.count(pIrVol.first->qualifier) == 0) {
            qualifiers.insert(pIrVol.first->qualifier);
        }
        ++pIrVol.first;
    }
    // Inflation volatility qualifiers
    auto pInfVol = ssRiskTypeIndex.equal_range(make_tuple(nettingSetDetails, pc, RiskType::InflationVol));
    while (pInfVol.first != pInfVol.second) {
        if (qualifiers.count(pInfVol.first->qualifier) == 0) {
            qualifiers.insert(pInfVol.first->qualifier);
        }
        ++pInfVol.first;
    }

    // If there are no qualifiers, return early and set bool to false to indicate margin does not apply
    if (qualifiers.empty()) {
        bucketMargins["All"] = 0.0;
        return make_pair(bucketMargins, false);
    }

    // The curvature margin for each currency i.e. $K_b$ from SIMM docs
    map<string, Real> curvatureMargin;
    // The sum of the weighted sensitivities for each currency i.e. $\sum_{k}^K CVR_{b,k}$ from SIMM docs
    map<string, Real> sumWeightedSensis;
    // The sum of all weighted sensitivities across currencies and risk factors
    Real sumWs = 0.0;
    // The sum of the absolute value of weighted sensitivities across currencies and risk factors
    Real sumAbsWs = 0.0;

    // Loop over the qualifiers i.e. currencies
    for (const auto& qualifier : qualifiers) {
        // Pair of iterators to start and end of IRVol sensitivities with current qualifier
        auto pIrQualifier = ssQualifierIndex.equal_range(make_tuple(nettingSetDetails, pc, RiskType::IRVol, qualifier));

        // Pair of iterators to start and end of InflationVol sensitivities with current qualifier
        auto pInfQualifier =
            ssQualifierIndex.equal_range(make_tuple(nettingSetDetails, pc, RiskType::InflationVol, qualifier));

        // Calculate the margin piece for this qualifier i.e. $K_b$ from SIMM docs
        // Start with IRVol vs. IRVol components
        for (auto itOuter = pIrQualifier.first; itOuter != pIrQualifier.second; ++itOuter) {
            // Curvature weight i.e. $SF(t_{kj})$ from SIMM docs
            Real sfOuter = simmConfiguration_->curvatureWeight(RiskType::IRVol, itOuter->label1);
            // Curvature sensitivity i.e. $CVR_{ik}$ from SIMM docs
            Real wsOuter = sfOuter * (itOuter->amountUsd * multiplier);
            // Update weighted sensitivity sums
            sumWeightedSensis[qualifier] += wsOuter;
            sumWs += wsOuter;
            sumAbsWs += std::abs(wsOuter);
            // Add diagonal element to curvature margin
            curvatureMargin[qualifier] += wsOuter * wsOuter;
            // Add the cross elements to the curvature margin
            for (auto itInner = pIrQualifier.first; itInner != itOuter; ++itInner) {
                // Label1 level correlation i.e. $\rho_{k,l}$ from SIMM docs
                Real corr = simmConfiguration_->correlation(RiskType::IRVol, qualifier, itOuter->label1, "",
                                                            RiskType::IRVol, qualifier, itInner->label1, "");
                // Add cross element to curvature margin
                Real sfInner = simmConfiguration_->curvatureWeight(RiskType::IRVol, itInner->label1);
                Real wsInner = sfInner * (itInner->amountUsd * multiplier);
                curvatureMargin[qualifier] += 2 * corr * corr * wsOuter * wsInner;
            }
        }

        // Now deal with inflation component
        SimmVersion version = parseSimmVersion(simmConfiguration_->version());
        SimmVersion thresholdVersion = SimmVersion::V1_0;
        if (version > thresholdVersion) {
            // Weighted sensitivity i.e. $WS_{k,i}$ from SIMM docs
            Real infWs = 0.0;
            for (auto infIt = pInfQualifier.first; infIt != pInfQualifier.second; ++infIt) {
                // Curvature weight i.e. $SF(t_{kj})$ from SIMM docs
                Real infSf = simmConfiguration_->curvatureWeight(RiskType::InflationVol, infIt->label1);
                infWs += infSf * (infIt->amountUsd * multiplier);
            }
            // Update weighted sensitivity sums
            sumWeightedSensis[qualifier] += infWs;
            sumWs += infWs;
            sumAbsWs += std::abs(infWs);

            // Add diagonal element to curvature margin - there is only one element for inflationVol
            curvatureMargin[qualifier] += infWs * infWs;

            // Add the cross elements to the curvature margin against IRVol components.
            // There are no cross elements against InflationVol since we only have one element.
            for (auto irIt = pIrQualifier.first; irIt != pIrQualifier.second; ++irIt) {
                // Correlation i.e. $\rho_{k,l}$ from SIMM docs
                Real corr = simmConfiguration_->correlation(RiskType::InflationVol, qualifier, "", "", RiskType::IRVol,
                                                            qualifier, irIt->label1, "");
                // Add cross element to curvature margin
                Real irSf = simmConfiguration_->curvatureWeight(RiskType::IRVol, irIt->label1);
                Real irWs = irSf * (irIt->amountUsd * multiplier);
                curvatureMargin[qualifier] += 2 * corr * corr * infWs * irWs;
            }
        }

        // Finally have the value of $K_b$
        curvatureMargin[qualifier] = sqrt(max(curvatureMargin[qualifier], 0.0));
    }

    // If sum of absolute value of all individual curvature risks is zero, we can return 0.0
    if (close_enough(sumAbsWs, 0.0)) {
        bucketMargins["All"] = 0.0;
        return make_pair(bucketMargins, true);
    }

    // Now calculate final curvature margin by aggregating across currencies
    Real theta = min(sumWs / sumAbsWs, 0.0);

    Real margin = 0.0;
    for (auto itOuter = qualifiers.begin(); itOuter != qualifiers.end(); ++itOuter) {
        // Diagonal term
        margin += curvatureMargin.at(*itOuter) * curvatureMargin.at(*itOuter);
        // Cross terms
        Real sOuter =
            max(min(sumWeightedSensis.at(*itOuter), curvatureMargin.at(*itOuter)), -curvatureMargin.at(*itOuter));
        for (auto itInner = qualifiers.begin(); itInner != itOuter; ++itInner) {
            Real sInner =
                max(min(sumWeightedSensis.at(*itInner), curvatureMargin.at(*itInner)), -curvatureMargin.at(*itInner));
            Real corr =
                simmConfiguration_->correlation(RiskType::IRVol, *itOuter, "", "", RiskType::IRVol, *itInner, "", "");
            margin += 2.0 * sOuter * sInner * corr * corr;
        }
    }
    margin = sumWs + lambda(theta) * sqrt(max(margin, 0.0));

    for (const auto& m : curvatureMargin)
        bucketMargins[m.first] = m.second;

    Real scaling = simmConfiguration_->curvatureMarginScaling();
    Real totalCurvatureMargin = scaling * max(margin, 0.0);
    // TODO: Review, should we return the pre-scaled value instead?
    bucketMargins["All"] = totalCurvatureMargin;

    return make_pair(bucketMargins, true);
}

pair<map<string, Real>, bool> SimmCalculator::margin(const NettingSetDetails& nettingSetDetails, const ProductClass& pc,
                                                     const RiskType& rt, const SimmNetSensitivities& netRecords) const {
    
    // "Bucket" here refers to exposures under the CRIF qualifiers for FX (and IR) risk class, and CRIF buckets for
    // every other risk class.
    // For FX Delta margin, this refers to WS_k in Section B. "Structure of the methodology", 8.(b).
    // For FX Vega margin, this refers to VR_k in Section B., 10.(d).
    // For other risk type, the bucket margin is K_b in the corresponding subsections.
    map<string, Real> bucketMargins;

    bool riskClassIsFX = rt == RiskType::FX || rt == RiskType::FXVol;

    // Index on SIMM sensitivities in to risk type level
    auto& ssRiskTypeIndex = netRecords.get<RiskTypeTag>();
    // Index on SIMM sensitivities in to bucket level
    auto& ssBucketIndex = netRecords.get<BucketTag>();
    // Index on SIMM sensitivities in to qualifier level
    auto& ssQualifierIndex = netRecords.get<BucketQualifierTag>();

    // Find the set of buckets and associated qualifiers for the netting set details, product class and risk type
    map<string, set<string>> buckets;
    auto p = ssRiskTypeIndex.equal_range(make_tuple(nettingSetDetails, pc, rt));
    while (p.first != p.second) {
        buckets[p.first->bucket].insert(p.first->qualifier);
        ++p.first;
    }

    // If there are no buckets, return early and set bool to false to indicate margin does not apply
    if (buckets.empty()) {
        bucketMargins["All"] = 0.0;
        return make_pair(bucketMargins, false);
    }

    // The margin for each bucket i.e. $K_b$ from SIMM docs
    map<string, Real> bucketMargin;
    // The sum of the weighted sensitivities for each bucket i.e. $\sum_{k=1}^{K} WS_{k}$ from SIMM docs
    map<string, Real> sumWeightedSensis;
    // The historical volatility ratio for the risk type - will be 1.0 if not applicable
    Real hvr = simmConfiguration_->historicalVolatilityRatio(rt);

    // Loop over the buckets
    for (const auto& kv : buckets) {
        string bucket = kv.first;

        // Initialise sumWeightedSensis here to ensure it is not empty in the later calculations
        sumWeightedSensis[bucket] = 0.0;

        // Get the concentration risk for each qualifier in current bucket i.e. $CR_k$ from SIMM docs
        map<string, Real> concentrationRisk;
        for (const auto& qualifier : buckets.at(bucket)) {

            // Do not include Risk_FX components in the calculation currency in the SIMM calculation
            if (rt == RiskType::FX && qualifier == calculationCcy_) {
                if (!quiet_) {
                    DLOG("Not calculating concentration risk for qualifier "
                         << qualifier << " of risk type " << rt
                         << " since the qualifier equals the SIMM calculation currency " << calculationCcy_);
                }
                continue;
            }

            // Pair of iterators to start and end of sensitivities with current qualifier
            auto pQualifier = ssQualifierIndex.equal_range(make_tuple(nettingSetDetails, pc, rt, bucket, qualifier));

            // One pass to get the concentration risk for this qualifier
            for (auto it = pQualifier.first; it != pQualifier.second; ++it) {
                // Get the sigma value if applicable - returns 1.0 if not applicable
                Real sigma = simmConfiguration_->sigma(rt, it->qualifier, it->label1, calculationCcy_);
                concentrationRisk[qualifier] += it->amountUsd * sigma * hvr;
            }
            // Divide by the concentration risk threshold
            concentrationRisk[qualifier] /= simmConfiguration_->concentrationThreshold(rt, qualifier);
            // Final concentration risk amount
            concentrationRisk[qualifier] = max(1.0, sqrt(std::abs(concentrationRisk[qualifier])));
        }

        // Calculate the margin component for the current bucket
        // Pair of iterators to start and end of sensitivities within current bucket
        auto pBucket = ssBucketIndex.equal_range(make_tuple(nettingSetDetails, pc, rt, bucket));
        for (auto itOuter = pBucket.first; itOuter != pBucket.second; ++itOuter) {
            // Do not include Risk_FX components in the calculation currency in the SIMM calculation
            if (rt == RiskType::FX && itOuter->qualifier == calculationCcy_) {
                if (!quiet_) {
                    DLOG("Skipping qualifier " << itOuter->qualifier << " of risk type " << rt
                                               << " since the qualifier equals the SIMM calculation currency "
                                               << calculationCcy_);
                }
                continue;
            }
            // Risk weight i.e. $RW_k$ from SIMM docs
            Real rwOuter = simmConfiguration_->weight(rt, itOuter->qualifier, itOuter->label1, calculationCcy_);
            // Get the sigma value if applicable - returns 1.0 if not applicable
            Real sigmaOuter = simmConfiguration_->sigma(rt, itOuter->qualifier, itOuter->label1, calculationCcy_);
            // Weighted sensitivity i.e. $WS_{k}$ from SIMM docs
            Real wsOuter = rwOuter * (itOuter->amountUsd * sigmaOuter * hvr) * concentrationRisk[itOuter->qualifier];
            // Update weighted sensitivity sum
            sumWeightedSensis[bucket] += wsOuter;
            // Add diagonal element to bucket margin
            bucketMargin[bucket] += wsOuter * wsOuter;
            // Add the cross elements to the bucket margin
            for (auto itInner = pBucket.first; itInner != itOuter; ++itInner) {
                // Do not include Risk_FX components in the calculation currency in the SIMM calculation
                if (rt == RiskType::FX && itInner->qualifier == calculationCcy_) {
                    if (!quiet_) {
                        DLOG("Skipping qualifier " << itInner->qualifier << " of risk type " << rt
                                                   << " since the qualifier equals the SIMM calculation currency "
                                                   << calculationCcy_);
                    }
                    continue;
                }
                // Correlation, $\rho_{k,l}$ in the SIMM docs
                Real corr = simmConfiguration_->correlation(rt, itOuter->qualifier, itOuter->label1, itOuter->label2,
                                                            rt, itInner->qualifier, itInner->label1, itInner->label2,
                                                            calculationCcy_);
                // $f_{k,l}$ from the SIMM docs
                Real f = min(concentrationRisk.at(itOuter->qualifier), concentrationRisk.at(itInner->qualifier)) /
                         max(concentrationRisk.at(itOuter->qualifier), concentrationRisk.at(itInner->qualifier));
                // Add cross element to delta margin
                Real sigmaInner = simmConfiguration_->sigma(rt, itInner->qualifier, itInner->label1, calculationCcy_);
                Real rwInner = simmConfiguration_->weight(rt, itInner->qualifier, itInner->label1, calculationCcy_);
                Real wsInner =
                    rwInner * (itInner->amountUsd * sigmaInner * hvr) * concentrationRisk[itInner->qualifier];
                bucketMargin[bucket] += 2 * corr * f * wsOuter * wsInner;
            }
            // For FX risk class, results are broken down by qualifier, i.e. currency, instead of bucket, which is not used for Risk_FX
            if (riskClassIsFX)
                bucketMargins[itOuter->qualifier] += wsOuter;
        }

        // Finally have the value of $K_b$
        bucketMargin[bucket] = sqrt(max(bucketMargin[bucket], 0.0));
    }

    // If there is a "Residual" bucket entry store it separately
    // This is $K_{residual}$ from SIMM docs
    Real residualMargin = 0.0;
    if (bucketMargin.count("Residual") > 0) {
        residualMargin = bucketMargin.at("Residual");
        bucketMargin.erase("Residual");
    }

    // Now calculate final margin by aggregating across non-residual buckets
    Real margin = 0.0;
    for (auto itOuter = bucketMargin.begin(); itOuter != bucketMargin.end(); ++itOuter) {
        string outerBucket = itOuter->first;
        // Diagonal term, $K_b^2$ from SIMM docs
        margin += itOuter->second * itOuter->second;
        // Cross terms
        // $S_b$ from SIMM docs
        Real sOuter = max(min(sumWeightedSensis.at(outerBucket), itOuter->second), -itOuter->second);
        for (auto itInner = bucketMargin.begin(); itInner != itOuter; ++itInner) {
            string innerBucket = itInner->first;
            // $S_c$ from SIMM docs
            Real sInner = max(min(sumWeightedSensis.at(innerBucket), itInner->second), -itInner->second);
            // $\gamma_{b,c}$ from SIMM docs
            // Interface to SimmConfiguration is on qualifiers => take any qualifier from each
            // of the respective (different) buckets to get the inter-bucket correlation
            string innerQualifier = *buckets.at(innerBucket).begin();
            string outerQualifier = *buckets.at(outerBucket).begin();
            Real corr = simmConfiguration_->correlation(rt, outerQualifier, "", "", rt, innerQualifier, "", "",
                                                        calculationCcy_);
            margin += 2.0 * sOuter * sInner * corr;
        }
    }
    margin = sqrt(max(margin, 0.0));

    // Now add the residual component back in
    margin += residualMargin;
    if (!close_enough(residualMargin, 0.0))
        bucketMargins["Residual"] = residualMargin;

    // For non-FX risk class, results are broken down by buckets
    if (!riskClassIsFX)
        for (const auto& m : bucketMargin)
            bucketMargins[m.first] = m.second;
    else
        for (auto& m : bucketMargins)
            m.second = std::abs(m.second);

    bucketMargins["All"] = margin;
    return make_pair(bucketMargins, true);
}

pair<map<string, Real>, bool>
SimmCalculator::curvatureMargin(const NettingSetDetails& nettingSetDetails, const ProductClass& pc, const RiskType& rt,
                                const SimmSide& side, const SimmNetSensitivities& netRecords, bool rfLabels) const {

    // "Bucket" here refers to exposures under the CRIF qualifiers for FX (and IR) risk class, and CRIF buckets for
    // every other risk class
    // For FX Curvature marrgin, this refers to CVR_{b,k} in Section B. "Structure of the methodology", 11.(c).
    // For other risk types, the bucket margin is K_b in the corresponding subsection.
    map<string, Real> bucketMargins;

    bool riskClassIsFX = rt == RiskType::FX || rt == RiskType::FXVol;

    // Multiplier for sensitivities, -1 if SIMM side is Post
    Real multiplier = side == SimmSide::Call ? 1.0 : -1.0;

    // Index on SIMM sensitivities in to risk type level
    auto& ssRiskTypeIndex = netRecords.get<RiskTypeTag>();
    // Index on SIMM sensitivities in to bucket level
    auto& ssBucketIndex = netRecords.get<BucketTag>();

    // Find the set of buckets and associated qualifiers for the portfolio, product class and risk type
    map<string, set<string>> buckets;
    auto p = ssRiskTypeIndex.equal_range(make_tuple(nettingSetDetails, pc, rt));
    while (p.first != p.second) {
        buckets[p.first->bucket].insert(p.first->qualifier);
        ++p.first;
    }

    // If there are no buckets, return early and set bool to false to indicate margin does not apply
    if (buckets.empty()) {
        bucketMargins["All"] = 0.0;
        return make_pair(bucketMargins, false);
    }

    // The curvature margin for each bucket i.e. $K_b$ from SIMM docs
    map<string, Real> curvatureMargin;
    // The sum of the weighted (and absolute weighted) sensitivities for each bucket
    // i.e. $\sum_{k}^K CVR_{b,k}$ from SIMM docs
    map<string, Real> sumWeightedSensis;
    map<string, map<string, Real>> sumAbsTemp;
    map<string, Real> sumAbsWeightedSensis;

    // Loop over the buckets
    for (const auto& kv : buckets) {
        string bucket = kv.first;
        sumAbsTemp[bucket] = {};

        // Calculate the margin component for the current bucket
        // Pair of iterators to start and end of sensitivities within current bucket
        auto pBucket = ssBucketIndex.equal_range(make_tuple(nettingSetDetails, pc, rt, bucket));
        for (auto itOuter = pBucket.first; itOuter != pBucket.second; ++itOuter) {
            // Curvature weight i.e. $SF(t_{kj})$ from SIMM docs
            Real sfOuter = simmConfiguration_->curvatureWeight(rt, itOuter->label1);
            // Get the sigma value if applicable - returns 1.0 if not applicable
            Real sigmaOuter = simmConfiguration_->sigma(rt, itOuter->qualifier, itOuter->label1, calculationCcy_);
            // Weighted curvature i.e. $CVR_{ik}$ from SIMM docs
            // WARNING: The order of multiplication here is important because unit tests fail if for
            //          example you use sfOuter * (itOuter->amountUsd * multiplier) * sigmaOuter;
            Real wsOuter = sfOuter * ((itOuter->amountUsd * multiplier) * sigmaOuter);
            // for ISDA SIMM 2.2 or higher, this $CVR_{ik}$ for EQ bucket 12 is zero
            SimmVersion version = parseSimmVersion(simmConfiguration_->version());
            SimmVersion thresholdVersion = SimmVersion::V2_2;
            if (version >= thresholdVersion && bucket == "12" && rt == RiskType::EquityVol) {
                wsOuter = 0.0;
            }
            // Update weighted sensitivity sum
            sumWeightedSensis[bucket] += wsOuter;
            sumAbsTemp[bucket][itOuter->qualifier] += rfLabels ? std::abs(wsOuter) : wsOuter;
            // Add diagonal element to curvature margin
            curvatureMargin[bucket] += wsOuter * wsOuter;
            // Add the cross elements to the curvature margin
            for (auto itInner = pBucket.first; itInner != itOuter; ++itInner) {
                // Correlation, $\rho_{k,l}$ in the SIMM docs
                Real corr = simmConfiguration_->correlation(rt, itOuter->qualifier, itOuter->label1, itOuter->label2,
                                                            rt, itInner->qualifier, itInner->label1, itInner->label2,
                                                            calculationCcy_);
                // Add cross element to delta margin
                Real sfInner = simmConfiguration_->curvatureWeight(rt, itInner->label1);
                Real sigmaInner = simmConfiguration_->sigma(rt, itInner->qualifier, itInner->label1, calculationCcy_);
                Real wsInner = sfInner * ((itInner->amountUsd * multiplier) * sigmaInner);
                curvatureMargin[bucket] += 2 * corr * corr * wsOuter * wsInner;
            }
            // For FX risk class, results are broken down by qualifier, i.e. currency, instead of bucket, which is not
            // used for Risk_FX
            if (riskClassIsFX)
                bucketMargins[itOuter->qualifier] += wsOuter;
        }

        // Finally have the value of $K_b$
        Real bucketCurvatureMargin = sqrt(max(curvatureMargin[bucket], 0.0));
        curvatureMargin[bucket] = bucketCurvatureMargin;

        // Bucket level absolute sensitivity
        for (const auto& kv : sumAbsTemp[bucket]) {
            sumAbsWeightedSensis[bucket] += std::abs(kv.second);
        }
    }

    // If there is a "Residual" bucket entry store it separately
    // This is $K_{residual}$ from SIMM docs
    Real residualMargin = 0.0;
    Real residualSum = 0.0;
    Real residualAbsSum = 0.0;
    if (curvatureMargin.count("Residual") > 0) {
        residualMargin = curvatureMargin.at("Residual");
        residualSum = sumWeightedSensis.at("Residual");
        residualAbsSum = sumAbsWeightedSensis.at("Residual");
        // Remove the entries for "Residual" bucket
        curvatureMargin.erase("Residual");
        sumWeightedSensis.erase("Residual");
        sumAbsWeightedSensis.erase("Residual");
    }

    // Now calculate final margin
    Real margin = 0.0;

    // First, aggregating across non-residual buckets
    auto acc = [](const Real p, const pair<const string, Real>& kv) { return p + kv.second; };
    Real sumSensis = accumulate(sumWeightedSensis.begin(), sumWeightedSensis.end(), 0.0, acc);
    Real sumAbsSensis = accumulate(sumAbsWeightedSensis.begin(), sumAbsWeightedSensis.end(), 0.0, acc);

    if (!close_enough(sumAbsSensis, 0.0)) {
        Real theta = min(sumSensis / sumAbsSensis, 0.0);
        for (auto itOuter = curvatureMargin.begin(); itOuter != curvatureMargin.end(); ++itOuter) {
            string outerBucket = itOuter->first;
            // Diagonal term
            margin += itOuter->second * itOuter->second;
            // Cross terms
            // $S_b$ from SIMM docs
            Real sOuter = max(min(sumWeightedSensis.at(outerBucket), itOuter->second), -itOuter->second);
            for (auto itInner = curvatureMargin.begin(); itInner != itOuter; ++itInner) {
                string innerBucket = itInner->first;
                // $S_c$ from SIMM docs
                Real sInner = max(min(sumWeightedSensis.at(innerBucket), itInner->second), -itInner->second);
                // $\gamma_{b,c}$ from SIMM docs
                // Interface to SimmConfiguration is on qualifiers => take any qualifier from each
                // of the respective (different) buckets to get the inter-bucket correlation
                string innerQualifier = *buckets.at(innerBucket).begin();
                string outerQualifier = *buckets.at(outerBucket).begin();
                Real corr = simmConfiguration_->correlation(rt, outerQualifier, "", "", rt, innerQualifier, "", "",
                                                            calculationCcy_);
                margin += 2.0 * sOuter * sInner * corr * corr;
            }
        }
        margin = max(sumSensis + lambda(theta) * sqrt(max(margin, 0.0)), 0.0);
    }

    // Second, the residual bucket if necessary, and add "Residual" bucket back in to be added to the SIMM results
    if (!close_enough(residualAbsSum, 0.0)) {
        Real theta = min(residualSum / residualAbsSum, 0.0);
        curvatureMargin["Residual"] = max(residualSum + lambda(theta) * residualMargin, 0.0);
        margin += curvatureMargin["Residual"];
    }

    // For non-FX risk class, results are broken down by buckets
    if (!riskClassIsFX)
        for (const auto& m : curvatureMargin)
            bucketMargins[m.first] = m.second;
    else
        for (auto& m : bucketMargins)
            m.second = std::abs(m.second);

    bucketMargins["All"] = margin;
    return make_pair(bucketMargins, true);
}

void SimmCalculator::calcAddMargin(const SimmSide& side, const NettingSetDetails& nettingSetDetails,
                                   const string& regulation, const SimmNetSensitivities& netRecords) {

    // Index on SIMM sensitivities in to risk type level
    auto& ssRiskTypeIndex = netRecords.get<RiskTypeTag>();

    // Reference to SIMM results for this portfolio
    auto& results = simmResults_[side][nettingSetDetails][regulation];

    const bool overwrite = false;

    if (!quiet_) {
        DLOG("Calculating additional margin for portfolio [" << nettingSetDetails << "], regulation " << regulation
                                                             << " and SIMM side " << side);
    }

    // First, add scaled additional margin, using "ProductClassMultiplier"
    // risk type, for the portfolio
    auto pc = ProductClass::Empty;
    auto rt = RiskType::ProductClassMultiplier;
    auto key = make_tuple(nettingSetDetails, pc, rt);
    auto pIt = ssRiskTypeIndex.equal_range(key);

    while (pIt.first != pIt.second) {
        // Qualifier should be a product class string
        auto qpc = parseSimmProductClass(pIt.first->qualifier);
        if (results.has(qpc, RiskClass::All, MarginType::All, "All")) {
            Real im = results.get(qpc, RiskClass::All, MarginType::All, "All");
            Real factor = pIt.first->amount;
            QL_REQUIRE(factor >= 0.0, "SIMM Calculator: Amount for risk type "
                << rt << " must be greater than or equal to 0 but we got " << factor);
            Real pcmMargin = (factor - 1.0) * im;
            add(nettingSetDetails, regulation, qpc, RiskClass::All, MarginType::AdditionalIM, "All", pcmMargin, side,
                overwrite);

            // Add to aggregation at margin type level
            add(nettingSetDetails, regulation, qpc, RiskClass::All, MarginType::All, "All", pcmMargin, side, overwrite);
            // Add to aggregation at product class level
            add(nettingSetDetails, regulation, ProductClass::All, RiskClass::All, MarginType::AdditionalIM, "All", pcmMargin,
                side, overwrite);
            // Add to aggregation at portfolio level
            add(nettingSetDetails, regulation, ProductClass::All, RiskClass::All, MarginType::All, "All", pcmMargin, side,
                overwrite);
        }
        ++pIt.first;
    }

    // Second, add fixed amounts IM, using "AddOnFixedAmount" risk type, for the portfolio
    rt = RiskType::AddOnFixedAmount;
    key = make_tuple(nettingSetDetails, pc, rt);
    pIt = ssRiskTypeIndex.equal_range(key);
    while (pIt.first != pIt.second) {
        Real fixedMargin = pIt.first->amountUsd;
        add(nettingSetDetails, regulation, ProductClass::AddOnFixedAmount, RiskClass::All, MarginType::AdditionalIM,
            "All", fixedMargin, side, overwrite);

        // Add to aggregation at margin type level
        add(nettingSetDetails, regulation, ProductClass::AddOnFixedAmount, RiskClass::All, MarginType::All, "All",
            fixedMargin,
            side, overwrite);
        // Add to aggregation at product class level
        add(nettingSetDetails, regulation, ProductClass::All, RiskClass::All, MarginType::AdditionalIM, "All",
            fixedMargin, side, overwrite);
        // Add to aggregation at portfolio level
        add(nettingSetDetails, regulation, ProductClass::All, RiskClass::All, MarginType::All, "All", fixedMargin, side,
            overwrite);
        ++pIt.first;
    }

    // Third, add percentage of notional amounts IM, using "AddOnNotionalFactor"
    // and "Notional" risk types, for the portfolio.

    // Index on SIMM sensitivities in to risk type level
    auto& ssQualifierIndex = netRecords.get<QualifierTag>();

    rt = RiskType::AddOnNotionalFactor;
    key = make_tuple(nettingSetDetails, pc, rt);
    pIt = ssRiskTypeIndex.equal_range(key);
    while (pIt.first != pIt.second) {
        // We should have a single corresponding CrifRecord with risk type
        // "Notional" and the same qualifier. Search for it.
        auto pQualifierIt = ssQualifierIndex.equal_range(
            make_tuple(nettingSetDetails, pc, RiskType::Notional, pIt.first->qualifier));
        auto count = distance(pQualifierIt.first, pQualifierIt.second);
        QL_REQUIRE(count < 2, "Expected either 0 or 1 elements for risk type "
                                    << RiskType::Notional << " and qualifier " << pIt.first->qualifier
                                    << " but got " << count);

        // If we have found a corresponding notional, update the additional margin
        if (count == 1) {
            Real notional = pQualifierIt.first->amountUsd;
            Real factor = pIt.first->amount;
            Real notionalFactorMargin = notional * factor / 100.0;

            add(nettingSetDetails, regulation, ProductClass::AddOnNotionalFactor, RiskClass::All,
                MarginType::AdditionalIM, "All", notionalFactorMargin, side, overwrite);

            // Add to aggregation at margin type level
            add(nettingSetDetails, regulation, ProductClass::AddOnNotionalFactor, RiskClass::All, MarginType::All,
                "All",
                notionalFactorMargin, side, overwrite);
            // Add to aggregation at product class level
            add(nettingSetDetails, regulation, ProductClass::All, RiskClass::All, MarginType::AdditionalIM, "All",
                notionalFactorMargin, side, overwrite);
            // Add to aggregation at portfolio level
            add(nettingSetDetails, regulation, ProductClass::All, RiskClass::All, MarginType::All, "All",
                notionalFactorMargin,
                side, overwrite);
        }

        ++pIt.first;
    }
}

void SimmCalculator::populateResults(const SimmSide& side, const NettingSetDetails& nettingSetDetails,
                                     const string& regulation) {

    if (!quiet_) {
        LOG("SimmCalculator: Populating higher level results")
    }

    // Sets of classes (excluding 'All')
    auto pcs = simmConfiguration_->productClasses(false);
    auto rcs = simmConfiguration_->riskClasses(false);
    auto mts = simmConfiguration_->marginTypes(false);

    // Populate netting set level results for each portfolio

    // Reference to SIMM results for this portfolio
    auto& results = simmResults_[side][nettingSetDetails][regulation];

    // Fill in the margin within each (product class, risk class) combination
    for (const auto& pc : pcs) {
        for (const auto& rc : rcs) {
            // Margin for a risk class is just sum over margin for each margin type
            // within that risk class
            Real riskClassMargin = 0.0;
            bool hasRiskClass = false;
            for (const auto& mt : mts) {
                if (results.has(pc, rc, mt, "All")) {
                    riskClassMargin += results.get(pc, rc, mt, "All");
                    if (!hasRiskClass)
                        hasRiskClass = true;
                }
            }

            // Add the margin to the results if it was calculated
            if (hasRiskClass) {
                add(nettingSetDetails, regulation, pc, rc, MarginType::All, "All", riskClassMargin, side);
            }
        }
    }

    // Fill in the margin within each product class by aggregating across risk classes
    for (const auto& pc : pcs) {
        Real productClassMargin = 0.0;
        bool hasProductClass = false;

        // IM within product class across risk classes requires correlation
        // o suffix => outer and i suffix => inner here
        for (auto ito = rcs.begin(); ito != rcs.end(); ++ito) {
            // Skip to next if no results for current risk class
            if (!results.has(pc, *ito, MarginType::All, "All"))
                continue;
            // If we get to here, we have the current product class
            if (!hasProductClass)
                hasProductClass = true;

            Real imo = results.get(pc, *ito, MarginType::All, "All");
            // Diagonal term
            productClassMargin += imo * imo;

            // Now, cross terms
            for (auto iti = rcs.begin(); iti != ito; ++iti) {
                if (!results.has(pc, *iti, MarginType::All, "All"))
                    continue;
                Real imi = results.get(pc, *iti, MarginType::All, "All");
                // Get the correlation between risk classes
                Real corr = simmConfiguration_->correlationRiskClasses(*ito, *iti);
                productClassMargin += 2.0 * corr * imo * imi;
            }
        }

        // Add the margin to the results if it was calculated
        if (hasProductClass) {
            productClassMargin = sqrt(max(productClassMargin, 0.0));
            add(nettingSetDetails, regulation, pc, RiskClass::All, MarginType::All, "All", productClassMargin, side);
        }
    }

    // Overall initial margin for the portfolio is the sum of the initial margin in
    // each of the product classes. Could have done it in the last loop but cleaner here.
    Real im = 0.0;
    for (const auto& pc : pcs) {
        if (results.has(pc, RiskClass::All, MarginType::All, "All")) {
            im += results.get(pc, RiskClass::All, MarginType::All, "All");
        }
    }
    add(nettingSetDetails, regulation, ProductClass::All, RiskClass::All, MarginType::All, "All", im, side);

    // Combinations outside of the natural SIMM hierarchy

    // Across risk class, for each product class and margin type
    for (const auto& pc : pcs) {
        for (const auto& mt : mts) {
            Real margin = 0.0;
            bool hasPcMt = false;

            // IM within product class and margin type across risk classes
            // requires correlation. o suffix => outer and i suffix => inner here
            for (auto ito = rcs.begin(); ito != rcs.end(); ++ito) {
                // Skip to next if no results for current risk class
                if (!results.has(pc, *ito, mt, "All"))
                    continue;
                // If we get to here, we have the current product class & margin type
                if (!hasPcMt)
                    hasPcMt = true;

                Real imo = results.get(pc, *ito, mt, "All");
                // Diagonal term
                margin += imo * imo;

                // Now, cross terms
                for (auto iti = rcs.begin(); iti != ito; ++iti) {
                    if (!results.has(pc, *iti, mt, "All"))
                        continue;
                    Real imi = results.get(pc, *iti, mt, "All");
                    // Get the correlation between risk classes
                    Real corr = simmConfiguration_->correlationRiskClasses(*ito, *iti);
                    margin += 2.0 * corr * imo * imi;
                }
            }

            // Add the margin to the results if it was calculated
            if (hasPcMt) {
                margin = sqrt(max(margin, 0.0));
                add(nettingSetDetails, regulation, pc, RiskClass::All, mt, "All", margin, side);
            }
        }
    }

    // Across product class, for each risk class and margin type
    for (const auto& rc : rcs) {
        for (const auto& mt : mts) {
            Real margin = 0.0;
            bool hasRcMt = false;

            // Can just sum across product class
            for (const auto& pc : pcs) {
                // Skip to next if no results for current product class
                if (!results.has(pc, rc, mt, "All"))
                    continue;
                // If we get to here, we have the current risk class & margin type
                if (!hasRcMt)
                    hasRcMt = true;

                margin += results.get(pc, rc, mt, "All");
            }

            // Add the margin to the results if it was calculated
            if (hasRcMt) {
                add(nettingSetDetails, regulation, ProductClass::All, rc, mt, "All", margin, side);
            }
        }
    }

    // Across product class and margin type for each risk class
    // Have already computed MarginType::All results above so just need
    // to sum over product class for each risk class here
    for (const auto& rc : rcs) {
        Real margin = 0.0;
        bool hasRc = false;

        for (const auto& pc : pcs) {
            // Skip to next if no results for current product class
            if (!results.has(pc, rc, MarginType::All, "All"))
                continue;
            // If we get to here, we have the current risk class
            if (!hasRc)
                hasRc = true;

            margin += results.get(pc, rc, MarginType::All, "All");
        }

        // Add the margin to the results if it was calculated
        if (hasRc) {
            add(nettingSetDetails, regulation, ProductClass::All, rc, MarginType::All, "All", margin, side);
        }
    }

    // Across product class and risk class for each margin type
    // Have already computed RiskClass::All results above so just need
    // to sum over product class for each margin type here
    for (const auto& mt : mts) {
        Real margin = 0.0;
        bool hasMt = false;

        for (const auto& pc : pcs) {
            // Skip to next if no results for current product class
            if (!results.has(pc, RiskClass::All, mt, "All"))
                continue;
            // If we get to here, we have the current risk class
            if (!hasMt)
                hasMt = true;

            margin += results.get(pc, RiskClass::All, mt, "All");
        }

        // Add the margin to the results if it was calculated
        if (hasMt) {
            add(nettingSetDetails, regulation, ProductClass::All, RiskClass::All, mt, "All", margin, side);
        }
    }
}

void SimmCalculator::populateFinalResults(const map<SimmSide, map<NettingSetDetails, string>>& winningRegs) {

    if (!quiet_) {
        LOG("SimmCalculator: Populating final winning regulators' IM");
    }
    winningRegulations_ = winningRegs;

    // Populate list of trade IDs of final trades used for SIMM winning reg
    for (auto& tids : finalTradeIds_)
        tids.second.clear();
    for (const auto& sv : winningRegs) {
        SimmSide side = sv.first;
        finalTradeIds_.insert(std::make_pair(side, std::set<string>()));
        
        for (const auto& nv : sv.second) {
            NettingSetDetails nsd = nv.first;
            string winningReg = nv.second;

            if (tradeIds_.count(side) > 0)
                if (tradeIds_.at(side).count(nsd) > 0)
                    if (tradeIds_.at(side).at(nsd).count(winningReg) > 0)
                        for (const string& tid : tradeIds_.at(side).at(nsd).at(winningReg))
                            finalTradeIds_.at(side).insert(tid);
        }
    }

    // Populate final SIMM results
    for (const auto& sv : simmResults_) {
        const SimmSide side = sv.first;

        for (const auto& nv : sv.second) {
            const NettingSetDetails& nsd = nv.first;

            const string& reg = winningRegulations(side, nsd);
            // If no results found for winning regulator, i.e IM is Schedule IM only, use empty SIMM results
            const SimmResults simmResults = nv.second.find(reg) == nv.second.end()
                                                ? SimmResults(resultCcy_)
                                                : nv.second.at(reg);
            finalSimmResults_[side][nsd] = make_pair(reg, simmResults);
        }
    }
}

void SimmCalculator::populateFinalResults() {
    populateFinalResults(winningRegulations_);
}

void SimmCalculator::add(const NettingSetDetails& nettingSetDetails, const string& regulation, const ProductClass& pc,
                         const RiskClass& rc, const MarginType& mt, const string& b, Real margin, SimmSide side,
                         const bool overwrite) {
    if (!quiet_) {
        DLOG("Calculated " << side << " margin for [netting set details, product class, risk class, margin type] = ["
                           << "[" << NettingSetDetails(nettingSetDetails) << "]"
                           << ", " << pc << ", " << rc << ", " << mt << "] of " << margin);
    }

    simmResults_[side][nettingSetDetails][regulation].add(pc, rc, mt, b, margin, "USD", calculationCcy_, overwrite);
}

void SimmCalculator::add(const NettingSetDetails& nettingSetDetails, const string& regulation, const ProductClass& pc,
                         const RiskClass& rc, const MarginType& mt, const map<string, Real>& margins, SimmSide side,
                         const bool overwrite) {

    for (const auto& kv : margins)
        add(nettingSetDetails, regulation, pc, rc, mt, kv.first, kv.second, side, overwrite);
}

void SimmCalculator::addCrifRecord(const CrifRecord& crifRecord, const SimmSide& side,
                                   const bool enforceIMRegulations) {

    const NettingSetDetails& nettingSetDetails = crifRecord.nettingSetDetails;

    bool collectRegsIsEmpty = false;
    bool postRegsIsEmpty = false;
    if (collectRegsIsEmpty_.find(crifRecord.nettingSetDetails) != collectRegsIsEmpty_.end())
        collectRegsIsEmpty = collectRegsIsEmpty_.at(crifRecord.nettingSetDetails);
    if (postRegsIsEmpty_.find(crifRecord.nettingSetDetails) != postRegsIsEmpty_.end())
        postRegsIsEmpty = postRegsIsEmpty_.at(crifRecord.nettingSetDetails);

    string regsString;
    if (enforceIMRegulations)
        regsString = side == SimmSide::Call ? crifRecord.collectRegulations : crifRecord.postRegulations;
    set<string> regs = parseRegulationString(regsString);

    auto newCrifRecord = crifRecord;
    newCrifRecord.collectRegulations.clear();
    newCrifRecord.postRegulations.clear();
    for (const string& r : regs)
        if (r == "Excluded" ||
            (r == "Unspecified" && enforceIMRegulations && !(collectRegsIsEmpty && postRegsIsEmpty))) {
            continue;
        } else if (r != "Excluded") {
            // Keep a record of trade IDs for each regulation
            if (!newCrifRecord.isSimmParameter())
                tradeIds_[side][nettingSetDetails][r].insert(newCrifRecord.tradeId);

            // Add CRIF record to the appropriate regulations
            if (regSensitivities_[side][nettingSetDetails][r] == nullptr)
                regSensitivities_[side][nettingSetDetails][r] = boost::make_shared<CrifLoader>(simmConfiguration_, CrifRecord::additionalHeaders, true, true);

            // We make sure to ignore amountCcy when aggregating the records, since we will only be using amountUsd,
            // and we may have CRIF records that are equal everywhere except for the amountCcy, and this will fail
            // in the case of Risk_XCcyBasis and Risk_Inflation.
            const bool onDiffAmountCcy = true;
            regSensitivities_[side][nettingSetDetails][r]->add(newCrifRecord, onDiffAmountCcy);
        }
}

Real SimmCalculator::lambda(Real theta) const {
    // Use boost inverse normal here as opposed to QL. Using QL inverse normal
    // will cause the ISDA SIMM unit tests to fail
    static Real q = boost::math::quantile(boost::math::normal(), 0.995);

    return (q * q - 1.0) * (1.0 + theta) - theta;
}

void SimmCalculator::convert() {
    // If calculation currency is USD, nothing to do.
    if (resultCcy_ == "USD")
        return;

    QL_REQUIRE(market_, "market not set");
    QuantLib::Handle<QuantLib::Quote> fxQuote = market_->fxRate("USD" + resultCcy_);
    QL_REQUIRE(!fxQuote.empty(), "market FX/USD/" << resultCcy_ << " rate not found");
    const Real fxSpot = fxQuote->value();

    QL_REQUIRE(fxSpot > 0, "SIMM Calculator: The USD spot rate must be positive");

    // Loop over all results and divide by the spot rate (i.e. convert from USD to SIMM calculation currency)
    for (auto& side : simmResults_) {
        for (auto& okv : side.second) {
            for (auto& reg : okv.second) {
                reg.second.convert(fxSpot, resultCcy_);
            }
        }
    }
}

} // namespace analytics
} // namespace ore
