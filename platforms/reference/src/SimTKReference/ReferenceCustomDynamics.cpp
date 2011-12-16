
/* Portions copyright (c) 2011 Stanford University and Simbios.
 * Contributors: Peter Eastman
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS, CONTRIBUTORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "../SimTKUtilities/SimTKOpenMMCommon.h"
#include "../SimTKUtilities/SimTKOpenMMLog.h"
#include "../SimTKUtilities/SimTKOpenMMUtilities.h"
#include "openmm/OpenMMException.h"
#include "lepton/Parser.h"
#include "ReferenceCustomDynamics.h"
#include "lepton/ParsedExpression.h"
#include "openmm/internal/ContextImpl.h"
#include "openmm/internal/ForceImpl.h"
#include "lepton/Operation.h"
#include <set>

using namespace std;
using namespace OpenMM;

/**---------------------------------------------------------------------------------------

   ReferenceCustomDynamics constructor

   @param numberOfAtoms  number of atoms
   @param integrator     the integrator definition to use

   --------------------------------------------------------------------------------------- */

ReferenceCustomDynamics::ReferenceCustomDynamics(int numberOfAtoms, const CustomIntegrator& integrator) : 
           ReferenceDynamics(numberOfAtoms, integrator.getStepSize(), 0.0), integrator(integrator) {
   sumBuffer.resize(numberOfAtoms);
   stepType.resize(integrator.getNumComputations());
   stepVariable.resize(integrator.getNumComputations());
   stepExpression.resize(integrator.getNumComputations());
   for (int i = 0; i < integrator.getNumComputations(); i++) {
       string expression;
       integrator.getComputationStep(i, stepType[i], stepVariable[i], expression);
       if (expression.length() > 0)
           stepExpression[i] = Lepton::Parser::parse(expression).createProgram();
   }
}

/**---------------------------------------------------------------------------------------

   ReferenceCustomDynamics destructor

   --------------------------------------------------------------------------------------- */

ReferenceCustomDynamics::~ReferenceCustomDynamics() {
}

/**---------------------------------------------------------------------------------------

   Update -- driver routine for performing Custom dynamics update of coordinates
   and velocities

   @param context             the context this integrator is updating
   @param numberOfAtoms       number of atoms
   @param atomCoordinates     atom coordinates
   @param velocities          velocities
   @param forces              forces
   @param masses              atom masses
   @param globals             a map containing values of global variables
   @param forcesAreValid      whether the current forces are valid or need to be recomputed

   --------------------------------------------------------------------------------------- */

void ReferenceCustomDynamics::update(ContextImpl& context, int numberOfAtoms, vector<RealVec>& atomCoordinates,
                                     vector<RealVec>& velocities, vector<RealVec>& forces, vector<RealOpenMM>& masses,
                                     map<string, RealOpenMM>& globals, vector<vector<RealVec> >& perDof, bool& forcesAreValid){
    int numSteps = stepType.size();
    globals.insert(context.getParameters().begin(), context.getParameters().end());
    if (invalidatesForces.size() == 0) {
        // The first time this is called, work out when to recompute forces and energy.  First build a
        // list of every step that invalidates the forces.
        
        invalidatesForces.resize(numSteps, false);
        needsForces.resize(numSteps, false);
        needsEnergy.resize(numSteps, false);
        set<string> affectsForce;
        affectsForce.insert("x");
        for (vector<ForceImpl*>::const_iterator iter = context.getForceImpls().begin(); iter != context.getForceImpls().end(); ++iter) {
            const map<string, double> params = (*iter)->getDefaultParameters();
            for (map<string, double>::const_iterator param = params.begin(); param != params.end(); ++param)
                affectsForce.insert(param->first);
        }
        for (int i = 0; i < numSteps; i++)
            invalidatesForces[i] = (stepType[i] == CustomIntegrator::ConstrainPositions || affectsForce.find(stepVariable[i]) != affectsForce.end());
        
        // Make a list of which steps require valid forces or energy to be known.
        
        for (int i = 0; i < numSteps; i++) {
            if (stepType[i] == CustomIntegrator::ComputeGlobal || stepType[i] == CustomIntegrator::ComputePerDof || stepType[i] == CustomIntegrator::ComputeSum) {
                for (int j = 0; j < stepExpression[i].getNumOperations(); j++) {
                    const Lepton::Operation& op = stepExpression[i].getOperation(j);
                    if (op.getId() == Lepton::Operation::VARIABLE) {
                        if (op.getName() == "f")
                            needsForces[i] = true;
                        else if (op.getName() == "energy")
                            needsEnergy[i] = true;
                    }
                }
            }
        }
        
        // Build the list of inverse masses.
        
        inverseMasses.resize(numberOfAtoms);
        for (int i = 0; i < numberOfAtoms; i++)
            inverseMasses[i] = 1.0/masses[i];
    }
    
    // Loop over steps and execute them.
    
    for (int i = 0; i < numSteps; i++) {
        if ((needsForces[i] || needsEnergy[i]) && !forcesAreValid) {
            // Recompute forces and or energy.  Figure out what is actually needed
            // between now and the next time they get invalidated again.
            
            bool computeForce = false, computeEnergy = false;
            for (int j = i; ; j++) {
                if (needsForces[j])
                    computeForce = true;
                if (needsEnergy[j])
                    computeEnergy = true;
                if (invalidatesForces[j])
                    break;
                if (j == numSteps-1)
                    j = -1;
                if (j == i-1)
                    break;
            }
            recordChangedParameters(context, globals);
            RealOpenMM e = context.calcForcesAndEnergy(computeForce, computeEnergy);
            if (computeEnergy)
                energy = e;
            forcesAreValid = true;
        }
        globals["energy"] = energy;
        
        // Execute the step.
        
        switch (stepType[i]) {
            case CustomIntegrator::ComputeGlobal: {
                map<string, RealOpenMM> variables = globals;
                variables["uniform"] = SimTKOpenMMUtilities::getUniformlyDistributedRandomNumber();
                variables["gaussian"] = SimTKOpenMMUtilities::getNormallyDistributedRandomNumber();
                globals[stepVariable[i]] = stepExpression[i].evaluate(variables);
                break;
            }
            case CustomIntegrator::ComputePerDof: {
                vector<RealVec>* results = NULL;
                if (stepVariable[i] == "x")
                    results = &atomCoordinates;
                else if (stepVariable[i] == "v")
                    results = &velocities;
                else {
                    for (int j = 0; j < integrator.getNumPerDofVariables(); j++)
                        if (stepVariable[i] == integrator.getPerDofVariableName(j))
                            results = &perDof[j];
                }
                if (results == NULL)
                    throw OpenMMException("Illegal per-DOF output variable: "+stepVariable[i]);
                computePerDof(numberOfAtoms, *results, atomCoordinates, velocities, forces, masses, globals, perDof, stepExpression[i]);
                break;
            }
            case CustomIntegrator::ComputeSum: {
                computePerDof(numberOfAtoms, sumBuffer, atomCoordinates, velocities, forces, masses, globals, perDof, stepExpression[i]);
                RealOpenMM sum = 0.0;
                for (int j = 0; j < numberOfAtoms; j++)
                    sum += sumBuffer[j][0]+sumBuffer[j][1]+sumBuffer[j][2];
                globals[stepVariable[i]] = sum;
                break;
            }
            case CustomIntegrator::ConstrainPositions: {
                getReferenceConstraintAlgorithm()->apply(numberOfAtoms, atomCoordinates, atomCoordinates, inverseMasses);
                break;
            }
            case CustomIntegrator::ConstrainVelocities: {
                getReferenceConstraintAlgorithm()->applyToVelocities(numberOfAtoms, atomCoordinates, velocities, inverseMasses);
                break;
            }
            case CustomIntegrator::UpdateContextState: {
                recordChangedParameters(context, globals);
                context.updateContextState();
                globals.insert(context.getParameters().begin(), context.getParameters().end());
            }
        }
        if (invalidatesForces[i])
            forcesAreValid = false;
    }
    incrementTimeStep();
    recordChangedParameters(context, globals);
}

RealOpenMM ReferenceCustomDynamics::computePerDof(int numberOfAtoms, vector<RealVec>& results, const vector<RealVec>& atomCoordinates,
              const vector<RealVec>& velocities, const vector<RealVec>& forces, const vector<RealOpenMM>& masses,
              const map<string, RealOpenMM>& globals, const vector<vector<RealVec> >& perDof, const Lepton::ExpressionProgram& expression) {
    // Loop over all degrees of freedom.
    
    map<string, RealOpenMM> variables = globals;
    for (int i = 0; i < numberOfAtoms; i++) {
        variables["m"] = masses[i];
        for (int j = 0; j < 3; j++) {
            // Compute the expression.
            
            variables["x"] = atomCoordinates[i][j];
            variables["v"] = velocities[i][j];
            variables["f"] = forces[i][j];
            variables["uniform"] = SimTKOpenMMUtilities::getUniformlyDistributedRandomNumber();
            variables["gaussian"] = SimTKOpenMMUtilities::getNormallyDistributedRandomNumber();
            for (int k = 0; k < (int) perDof.size(); k++)
                variables[integrator.getPerDofVariableName(k)] = perDof[k][i][j];
            results[i][j] = expression.evaluate(variables);
        }
    }
}

/**
 * Check which context parameters have changed and register them with the context.
 */
void ReferenceCustomDynamics::recordChangedParameters(OpenMM::ContextImpl& context, std::map<std::string, RealOpenMM>& globals) {
    for (map<string, double>::const_iterator iter = context.getParameters().begin(); iter != context.getParameters().end(); ++iter) {
        string name = iter->first;
        double value = globals[name];
        if (value != iter->second)
            context.setParameter(name, globals[name]);
    }
}
