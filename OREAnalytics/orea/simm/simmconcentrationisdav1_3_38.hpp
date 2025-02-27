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

/*! \file orea/simm/simmconcentrationisdav1_3_38.hpp
    \brief SIMM concentration thresholds for SIMM version 1.3.38
*/

#pragma once

#include <orea/simm/simmconcentration.hpp>
#include <orea/simm/simmbucketmapper.hpp>

#include <map>
#include <set>
#include <string>

namespace ore {
namespace analytics {

/*! This file used to be called simmconcentrationisdav338.hpp
    This class used to be called SimmConcentration_ISDA_V338

*/

//  Do not have documentation for these thresholds. They are copied from the
//  class SimmConfiguration_ISDA_V1_3_38 during refactor.

//! Class giving the SIMM concentration thresholds for v1.3.38
class SimmConcentration_ISDA_V1_3_38 : public SimmConcentrationBase {
public:
    //! Default constructor that adds fixed known mappings
    SimmConcentration_ISDA_V1_3_38(const boost::shared_ptr<SimmBucketMapper>& simmBucketMapper);

    /*! Return the SIMM <em>concentration threshold</em> for a given SIMM
        <em>RiskType</em> and SIMM <em>Qualifier</em>.

        \warning If the risk type is not covered <code>QL_MAX_REAL</code> is
                 returned i.e. no concentration threshold
     */
    QuantLib::Real threshold(const SimmConfiguration::RiskType& riskType, const std::string& qualifier) const override;

private:
    //! Help getting SIMM buckets from SIMM qualifiers
    boost::shared_ptr<SimmBucketMapper> simmBucketMapper_;
};

} // namespace analytics
} // namespace ore
