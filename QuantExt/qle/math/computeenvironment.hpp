/*
 Copyright (C) 2023 Quaternion Risk Management Ltd
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

/*! \file qle/math/computeenvironment.hpp
    \brief interface to compute envs
*/

#pragma once

#include <ql/patterns/singleton.hpp>

#include <cstdint>
#include <set>

namespace QuantExt {

class ComputeContext;
class ComputeFramework;

class ComputeEnvironment : public QuantLib::Singleton<ComputeEnvironment> {
public:
    ComputeEnvironment();
    ~ComputeEnvironment();
    std::set<std::string> getAvailableDevices() const;
    bool hasContext() const;
    void selectContext(const std::string& deviceName);
    ComputeContext& context();
    void reset();

private:
    void releaseFrameworks();

    std::vector<ComputeFramework*> frameworks_;
    ComputeContext* currentContext_;
};

class ComputeFramework {
public:
    virtual ~ComputeFramework() {}
    virtual std::set<std::string> getAvailableDevices() const = 0;
    virtual ComputeContext* getContext(const std::string& deviceName) = 0;
};

class ComputeContext {
public:
    struct DebugInfo {
        unsigned long numberOfOperations = 0;
        unsigned long nanoSecondsDataCopy = 0;
        unsigned long nanoSecondsProgramBuild = 0;
        unsigned long nanoSecondsCalculation = 0;
    };

    virtual ~ComputeContext() {}
    virtual void init() = 0;

    virtual std::pair<std::size_t, bool> initiateCalculation(const std::size_t n, const std::size_t id = 0,
                                                             const std::size_t version = 0,
                                                             const bool debug = false) = 0;

    virtual std::size_t createInputVariable(float v) = 0;
    virtual std::size_t createInputVariable(float* v) = 0;
    virtual std::vector<std::vector<std::size_t>> createInputVariates(const std::size_t dim, const std::size_t steps,
                                                                      const std::uint32_t seed) = 0;

    virtual std::size_t applyOperation(const std::size_t randomVariableOpCode,
                                       const std::vector<std::size_t>& args) = 0;
    virtual void freeVariable(const std::size_t id) = 0;
    virtual void declareOutputVariable(const std::size_t id) = 0;

    virtual void finalizeCalculation(std::vector<float*>& output) = 0;

    // debug info

    virtual const DebugInfo& debugInfo() const = 0;

    // convenience methods

    void finalizeCalculation(std::vector<std::vector<float>>& output);
};

} // namespace QuantExt
