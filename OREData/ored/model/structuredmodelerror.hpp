/*
 Copyright (C) 2020 Quaternion Risk Management Ltd
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

/*! \file ored/model/structuredmodelerror.hpp
    \brief Error for model calibration / building
    \ingroup marketdata
*/

#pragma once

#include <boost/algorithm/string.hpp>
#include <ored/utilities/log.hpp>

namespace ore {
namespace data {

//! Utility class for Structured Model errors
class StructuredModelErrorMessage : public StructuredMessage {
public:
    StructuredModelErrorMessage(const std::string& exceptionType, const std::string& exceptionWhat)
        : StructuredMessage(Category::Error, Group::Model, exceptionWhat,
                            std::map<std::string, std::string>({{"exceptionType", exceptionType}})) {}
};

} // namespace data
} // namespace ore
