/* -------------------------------------------------------------------------- *
 *                                   OpenMM                                   *
 * -------------------------------------------------------------------------- *
 * This is part of the OpenMM molecular simulation toolkit originating from   *
 * Simbios, the NIH National Center for Physics-Based Simulation of           *
 * Biological Structures at Stanford, funded under the NIH Roadmap for        *
 * Medical Research, grant U54 GM072970. See https://simtk.org.               *
 *                                                                            *
 * Portions copyright (c) 2008-2010 Stanford University and the Authors.      *
 * Authors: Peter Eastman                                                     *
 * Contributors:                                                              *
 *                                                                            *
 * Permission is hereby granted, free of charge, to any person obtaining a    *
 * copy of this software and associated documentation files (the "Software"), *
 * to deal in the Software without restriction, including without limitation  *
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,   *
 * and/or sell copies of the Software, and to permit persons to whom the      *
 * Software is furnished to do so, subject to the following conditions:       *
 *                                                                            *
 * The above copyright notice and this permission notice shall be included in *
 * all copies or substantial portions of the Software.                        *
 *                                                                            *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR *
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   *
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    *
 * THE AUTHORS, CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,    *
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR      *
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE  *
 * USE OR OTHER DEALINGS IN THE SOFTWARE.                                     *
 * -------------------------------------------------------------------------- */

/**
 * This tests the reference implementation of MonteCarloBarostat.
 */

#include "openmm/internal/AssertionUtilities.h"
#include "openmm/MonteCarloBarostat.h"
#include "openmm/Context.h"
#include "ReferencePlatform.h"
#include "openmm/NonbondedForce.h"
#include "openmm/System.h"
#include "openmm/LangevinIntegrator.h"
#include "openmm/VerletIntegrator.h"
#include "sfmt/SFMT.h"
#include "SimTKOpenMMRealType.h"
#include <iostream>
#include <vector>

using namespace OpenMM;
using namespace std;

void testChangingBoxSize() {
    ReferencePlatform platform;
    System system;
    system.setDefaultPeriodicBoxVectors(Vec3(4, 0, 0), Vec3(0, 5, 0), Vec3(0, 0, 6));
    system.addParticle(1.0);
    NonbondedForce* nb = new NonbondedForce();
    nb->setNonbondedMethod(NonbondedForce::CutoffPeriodic);
    nb->setCutoffDistance(2.0);
    nb->addParticle(1, 0.5, 0.5);
    system.addForce(nb);
    LangevinIntegrator integrator(300.0, 1.0, 0.01);
    Context context(system, integrator, platform);
    vector<Vec3> positions;
    positions.push_back(Vec3());
    context.setPositions(positions);
    Vec3 x, y, z;
    context.getState(State::Forces).getPeriodicBoxVectors(x, y, z);
    ASSERT_EQUAL_VEC(Vec3(4, 0, 0), x, 0);
    ASSERT_EQUAL_VEC(Vec3(0, 5, 0), y, 0);
    ASSERT_EQUAL_VEC(Vec3(0, 0, 6), z, 0);
    context.setPeriodicBoxVectors(Vec3(7, 0, 0), Vec3(0, 8, 0), Vec3(0, 0, 9));
    context.getState(State::Forces).getPeriodicBoxVectors(x, y, z);
    ASSERT_EQUAL_VEC(Vec3(7, 0, 0), x, 0);
    ASSERT_EQUAL_VEC(Vec3(0, 8, 0), y, 0);
    ASSERT_EQUAL_VEC(Vec3(0, 0, 9), z, 0);
    
    // Shrinking the box too small should produce an exception.
    
    context.setPeriodicBoxVectors(Vec3(7, 0, 0), Vec3(0, 3.9, 0), Vec3(0, 0, 9));
    bool ok = true;
    try {
        context.getState(State::Forces).getPeriodicBoxVectors(x, y, z);
        ok = false;
    }
    catch (exception& ex) {
    }
    ASSERT(ok);
}

void testIdealGas() {
    const int numParticles = 64;
    const int frequency = 10;
    const int steps = 1000;
    const double pressure = 1.5;
    const double pressureInMD = pressure*(AVOGADRO*1e-25); // pressure in kJ/mol/nm^3
    const double temp[] = {300.0, 600.0, 1000.0};
    const double initialVolume = numParticles*BOLTZ*temp[1]/pressureInMD;
    const double initialLength = std::pow(initialVolume, 1.0/3.0);

    // Create a gas of noninteracting particles.

    ReferencePlatform platform;
    System system;
    system.setDefaultPeriodicBoxVectors(Vec3(initialLength, 0, 0), Vec3(0, 0.5*initialLength, 0), Vec3(0, 0, 2*initialLength));
    vector<Vec3> positions(numParticles);
    OpenMM_SFMT::SFMT sfmt;
    init_gen_rand(0, sfmt);
    for (int i = 0; i < numParticles; ++i) {
        system.addParticle(1.0);
        positions[i] = Vec3(initialLength*genrand_real2(sfmt), 0.5*initialLength*genrand_real2(sfmt), 2*initialLength*genrand_real2(sfmt));
    }
    MonteCarloBarostat* barostat = new MonteCarloBarostat(pressure, temp[0], frequency);
    system.addForce(barostat);
    ASSERT(barostat->usesPeriodicBoundaryConditions());
    ASSERT(system.usesPeriodicBoundaryConditions());

    // Test it for three different temperatures.

    for (int i = 0; i < 3; i++) {
        barostat->setTemperature(temp[i]);
        LangevinIntegrator integrator(temp[i], 0.1, 0.01);
        Context context(system, integrator, platform);
        context.setPositions(positions);

        // Let it equilibrate.

        integrator.step(10000);

        // Now run it for a while and see if the volume is correct.

        double volume = 0.0;
        for (int j = 0; j < steps; ++j) {
            Vec3 box[3];
            context.getState(0).getPeriodicBoxVectors(box[0], box[1], box[2]);
            volume += box[0][0]*box[1][1]*box[2][2];
            ASSERT_EQUAL_TOL(0.5*box[0][0], box[1][1], 1e-5);
            ASSERT_EQUAL_TOL(2*box[0][0], box[2][2], 1e-5);
            integrator.step(frequency);
        }
        volume /= steps;
        double expected = (numParticles+1)*BOLTZ*temp[i]/pressureInMD;
        ASSERT_USUALLY_EQUAL_TOL(expected, volume, 3/std::sqrt((double) steps));
    }
}

void testRandomSeed() {
    const int numParticles = 8;
    const double temp = 100.0;
    const double pressure = 1.5;
    ReferencePlatform platform;
    System system;
    system.setDefaultPeriodicBoxVectors(Vec3(8, 0, 0), Vec3(0, 8, 0), Vec3(0, 0, 8));
    VerletIntegrator integrator(0.01);
    NonbondedForce* forceField = new NonbondedForce();
    forceField->setNonbondedMethod(NonbondedForce::CutoffPeriodic);
    for (int i = 0; i < numParticles; ++i) {
        system.addParticle(2.0);
        forceField->addParticle((i%2 == 0 ? 1.0 : -1.0), 1.0, 5.0);
    }
    system.addForce(forceField);
    MonteCarloBarostat* barostat = new MonteCarloBarostat(pressure, temp, 1);
    system.addForce(barostat);
    ASSERT(barostat->usesPeriodicBoundaryConditions());
    ASSERT(system.usesPeriodicBoundaryConditions());
    vector<Vec3> positions(numParticles);
    vector<Vec3> velocities(numParticles);
    for (int i = 0; i < numParticles; ++i) {
        positions[i] = Vec3((i%2 == 0 ? 2 : -2), (i%4 < 2 ? 2 : -2), (i < 4 ? 2 : -2));
        velocities[i] = Vec3(0, 0, 0);
    }

    // Try twice with the same random seed.

    barostat->setRandomNumberSeed(5);
    Context context(system, integrator, platform);
    context.setPositions(positions);
    context.setVelocities(velocities);
    integrator.step(10);
    State state1 = context.getState(State::Positions);
    context.reinitialize();
    context.setPositions(positions);
    context.setVelocities(velocities);
    integrator.step(10);
    State state2 = context.getState(State::Positions);

    // Try twice with a different random seed.

    barostat->setRandomNumberSeed(10);
    context.reinitialize();
    context.setPositions(positions);
    context.setVelocities(velocities);
    integrator.step(10);
    State state3 = context.getState(State::Positions);
    context.reinitialize();
    context.setPositions(positions);
    context.setVelocities(velocities);
    integrator.step(10);
    State state4 = context.getState(State::Positions);

    // Compare the results.

    for (int i = 0; i < numParticles; i++) {
        for (int j = 0; j < 3; j++) {
            ASSERT(state1.getPositions()[i][j] == state2.getPositions()[i][j]);
            ASSERT(state3.getPositions()[i][j] == state4.getPositions()[i][j]);
            ASSERT(state1.getPositions()[i][j] != state3.getPositions()[i][j]);
        }
    }
}

int main() {
    try {
        testChangingBoxSize();
        testIdealGas();
        testRandomSeed();
    }
    catch(const exception& e) {
        cout << "exception: " << e.what() << endl;
        return 1;
    }
    cout << "Done" << endl;
    return 0;
}

