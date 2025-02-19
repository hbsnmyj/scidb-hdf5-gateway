/*
**
* BEGIN_COPYRIGHT
*
* Copyright (C) 2008-2015 SciDB, Inc.
* All Rights Reserved.
*
* SciDB is free software: you can redistribute it and/or modify
* it under the terms of the AFFERO GNU General Public License as published by
* the Free Software Foundation.
*
* SciDB is distributed "AS-IS" AND WITHOUT ANY WARRANTY OF ANY KIND,
* INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY,
* NON-INFRINGEMENT, OR FITNESS FOR A PARTICULAR PURPOSE. See
* the AFFERO GNU General Public License for the complete license terms.
*
* You should have received a copy of the AFFERO GNU General Public License
* along with SciDB.  If not, see <http://www.gnu.org/licenses/agpl-3.0.html>
*
* END_COPYRIGHT
*/

/*
 * PhysicalSlice.cpp
 *
 *  Created on: May 6, 2010
 *      Author: Knizhnik
 */

#include <query/Operator.h>
#include <query/AutochunkFixer.h>
#include <array/Metadata.h>
#include <array/Array.h>
#include "SliceArray.h"

using namespace std;

namespace scidb {

class PhysicalSlice: public PhysicalOperator
{
public:
    PhysicalSlice(std::string const& logicalName,
                  std::string const& physicalName,
                  Parameters const& parameters,
                  ArrayDesc const& schema):
        PhysicalOperator(logicalName, physicalName, parameters, schema)
    {
    }

    virtual bool changesDistribution(std::vector<ArrayDesc> const&) const
    {
        return true;
    }

    virtual RedistributeContext getOutputDistribution(
            std::vector<RedistributeContext> const& inputDistributions,
            std::vector<ArrayDesc> const& inputSchemas) const
    {
        assertConsistency(inputSchemas[0], inputDistributions[0]);

        ArrayDesc* mySchema = const_cast<ArrayDesc*>(&_schema);
        SCIDB_ASSERT(_schema.getDistribution()->getPartitioningSchema()==psUndefined);
        mySchema->setResidency(inputDistributions[0].getArrayResidency());

        return RedistributeContext(_schema.getDistribution(),
                                   _schema.getResidency());
    }

    virtual PhysicalBoundaries getOutputBoundaries(
            std::vector<PhysicalBoundaries> const& inputBoundaries,
            std::vector< ArrayDesc> const& inputSchemas) const
    {
        if (inputBoundaries[0].isEmpty()) {
            return PhysicalBoundaries::createEmpty(_schema.getDimensions().size());
        }


        Coordinates newStart, newEnd;
        Coordinates inStart = inputBoundaries[0].getStartCoords();
        Coordinates inEnd = inputBoundaries[0].getEndCoords();
        Dimensions dims = inputSchemas[0].getDimensions();
        size_t nDims = dims.size();

        size_t nParams = _parameters.size();
        std::vector<std::string> sliceDimName(nParams/2);
        for (size_t i = 0; i < nParams; i+=2) {
            sliceDimName[i >> 1]  = ((std::shared_ptr<OperatorParamReference>&)_parameters[i])->getObjectName();
        }

        for (size_t i = 0; i < nDims; i++) {
            const std::string dimName = dims[i].getBaseName();
            int k = safe_static_cast<int>(sliceDimName.size());
            while (--k >= 0
                   && sliceDimName[k] != dimName
                   && !(sliceDimName[k][0] == '_' && (size_t)atoi(sliceDimName[k].c_str()+1) == i+1))
                ;

            if (k < 0) {
                //dimension i is present in output
                newStart.push_back(inStart[i]);
                newEnd.push_back(inEnd[i]);
            } else {
                //dimension i is not present in output; check value
                Coordinate slice = ((std::shared_ptr<OperatorParamPhysicalExpression>&)_parameters[k*2+1])->getExpression()->evaluate().getInt64();
                if (!inputBoundaries[0].isInsideBox(slice,i))
                {
                    //the slice value is outside the box; guess what - the result is an empty array
                    return PhysicalBoundaries::createEmpty(_schema.getDimensions().size());
                }
            }
        }

        // This does nothing but calculate a few local values
        // and then discard them.
        // 
        // double resultCells = PhysicalBoundaries::getNumCells(newStart, newEnd);
        // double origCells = inputBoundaries[0].getNumCells();
        // double newDensity = 1.0;
        // if (resultCells > 0.0)
        // {
        //     newDensity = inputBoundaries[0].getDensity() * origCells / resultCells;
        //     newDensity = newDensity > 1.0 ? 1.0 : newDensity;
        // }

        return PhysicalBoundaries(newStart, newEnd);
    }

    /// Get the stringified AutochunkFixer so we can fix up the intervals in execute().
    /// @see LogicalSlice::getInspectable()
    void inspectLogicalOp(LogicalOperator const& lop) override
    {
        setControlCookie(lop.getInspectable());
    }

    /**
      * Slice is a pipelined operator, hence it executes by returning
      * an iterator-based array to the consumer
      * that overrides the chunkiterator method.
      */
    std::shared_ptr< Array> execute(
            std::vector< std::shared_ptr< Array> >& inputArrays,
            std::shared_ptr<Query> query)
    {
        assert(inputArrays.size() == 1);

        AutochunkFixer af(getControlCookie());
        af.fix(_schema, inputArrays);

        std::shared_ptr<Array> inputArray = inputArrays[0];
        ArrayDesc const& desc = inputArray->getArrayDesc();
        inputArray = ensureRandomAccess(inputArray, query);
        Dimensions const& dims = desc.getDimensions();
        size_t nDims = dims.size();
        size_t nParams = _parameters.size();
        assert((nParams & 1) == 0 || nParams >= nDims*2);
        uint64_t mask = 0;
        Coordinates slice(nDims);
        vector<string> sliceDimName(nParams/2);

        // NOTE: may be to use already evaluated schema in logical operator from schema parameter
        for (size_t i = 0; i < nParams; i+=2) {
            assert(((std::shared_ptr<OperatorParam>&)_parameters[i])->getParamType() == PARAM_DIMENSION_REF);
            sliceDimName[i >> 1]  = ((std::shared_ptr<OperatorParamReference>&)_parameters[i])->getObjectName();
        }
        for (size_t i = 0; i < nDims; i++) {
            string dimName = dims[i].getBaseName();
            int k = safe_static_cast<int>(sliceDimName.size());
            while (--k >= 0
                   && sliceDimName[k] != dimName
                   && !(sliceDimName[k][0] == '_' && (size_t)atoi(sliceDimName[k].c_str()+1) == i+1));
            if (k >= 0) {
                assert(((std::shared_ptr<OperatorParam>&)_parameters[k*2+1])->getParamType() == PARAM_PHYSICAL_EXPRESSION);
                Value const& coord = ((std::shared_ptr<OperatorParamPhysicalExpression>&)_parameters[k*2+1])->getExpression()->evaluate();
                slice[i] = coord.getInt64();
                if (slice[i] < dims[i].getStartMin() || slice[i] > dims[i].getEndMax())
                    throw USER_EXCEPTION(SCIDB_SE_EXECUTION, SCIDB_LE_OP_SLICE_ERROR2);
                mask |= (uint64_t)1 << i;
            }
        }
        return std::shared_ptr<Array>( new SliceArray(_schema, slice, mask, inputArray));
    }
};

DECLARE_PHYSICAL_OPERATOR_FACTORY(PhysicalSlice, "slice", "physicalSlice")

}  // namespace scidb


